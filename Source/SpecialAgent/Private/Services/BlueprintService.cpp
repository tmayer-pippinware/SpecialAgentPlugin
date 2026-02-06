// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/BlueprintService.h"

#include "GameThreadDispatcher.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BlueprintEditorLibrary.h"
#include "EditorAssetLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "K2Node_CallFunction.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/EngineTypes.h"
#include "GameFramework/Actor.h"
#include "UObject/CoreNetTypes.h"
#include "UObject/UObjectIterator.h"
#include "Misc/PackageName.h"

namespace
{
	static TSharedPtr<FJsonObject> BuildNodeJson(const UEdGraphNode* Node)
	{
		TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		NodeObj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
		NodeObj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		NodeObj->SetNumberField(TEXT("x"), Node->NodePosX);
		NodeObj->SetNumberField(TEXT("y"), Node->NodePosY);

		TArray<TSharedPtr<FJsonValue>> PinsJson;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin)
			{
				continue;
			}

			TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
			PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
			PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
			PinObj->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
			if (Pin->PinType.PinSubCategory != NAME_None)
			{
				PinObj->SetStringField(TEXT("subcategory"), Pin->PinType.PinSubCategory.ToString());
			}
			if (Pin->PinType.PinSubCategoryObject.IsValid())
			{
				PinObj->SetStringField(TEXT("subcategory_object"), Pin->PinType.PinSubCategoryObject->GetPathName());
			}
			PinsJson.Add(MakeShared<FJsonValueObject>(PinObj));
		}

		NodeObj->SetArrayField(TEXT("pins"), PinsJson);
		return NodeObj;
	}

	static FName NormalizeEventName(const FString& EventName)
	{
		if (EventName.Equals(TEXT("BeginPlay"), ESearchCase::IgnoreCase))
		{
			return TEXT("ReceiveBeginPlay");
		}
		if (EventName.Equals(TEXT("Tick"), ESearchCase::IgnoreCase))
		{
			return TEXT("ReceiveTick");
		}
		return FName(*EventName);
	}

	static UClass* ResolveClassByNameOrPath(const FString& ClassNameOrPath)
	{
		if (ClassNameOrPath.IsEmpty())
		{
			return nullptr;
		}

		if (UClass* PathClass = FindObject<UClass>(nullptr, *ClassNameOrPath))
		{
			return PathClass;
		}
		if (UClass* LoadedPathClass = LoadObject<UClass>(nullptr, *ClassNameOrPath))
		{
			return LoadedPathClass;
		}

		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Candidate = *It;
			if (!Candidate)
			{
				continue;
			}
			if (Candidate->GetName().Equals(ClassNameOrPath, ESearchCase::CaseSensitive))
			{
				return Candidate;
			}
			if (Candidate->GetName().Equals(ClassNameOrPath, ESearchCase::IgnoreCase))
			{
				return Candidate;
			}
		}

		return nullptr;
	}

	template <typename TObjectType>
	static TObjectType* ResolveObjectByNameOrPath(const FString& NameOrPath)
	{
		if (NameOrPath.IsEmpty())
		{
			return nullptr;
		}

		if (TObjectType* ExistingObject = FindObject<TObjectType>(nullptr, *NameOrPath))
		{
			return ExistingObject;
		}
		if (TObjectType* LoadedObject = LoadObject<TObjectType>(nullptr, *NameOrPath))
		{
			return LoadedObject;
		}

		for (TObjectIterator<TObjectType> It; It; ++It)
		{
			TObjectType* Candidate = *It;
			if (!Candidate)
			{
				continue;
			}
			if (Candidate->GetName().Equals(NameOrPath, ESearchCase::CaseSensitive))
			{
				return Candidate;
			}
			if (Candidate->GetName().Equals(NameOrPath, ESearchCase::IgnoreCase))
			{
				return Candidate;
			}
		}

		return nullptr;
	}

	static FString PinContainerTypeToString(const EPinContainerType ContainerType)
	{
		switch (ContainerType)
		{
			case EPinContainerType::Array:
				return TEXT("array");
			case EPinContainerType::Set:
				return TEXT("set");
			case EPinContainerType::Map:
				return TEXT("map");
			default:
				return TEXT("none");
		}
	}

	static bool ParsePinContainerType(const FString& ContainerTypeName, EPinContainerType& OutContainerType, FString& OutError)
	{
		const FString Normalized = ContainerTypeName.TrimStartAndEnd().ToLower();

		if (Normalized.IsEmpty() || Normalized == TEXT("none"))
		{
			OutContainerType = EPinContainerType::None;
			return true;
		}
		if (Normalized == TEXT("array"))
		{
			OutContainerType = EPinContainerType::Array;
			return true;
		}
		if (Normalized == TEXT("set"))
		{
			OutContainerType = EPinContainerType::Set;
			return true;
		}
		if (Normalized == TEXT("map"))
		{
			OutContainerType = EPinContainerType::Map;
			return true;
		}

		OutError = FString::Printf(TEXT("Unsupported container_type '%s'. Supported: none, array, set, map"), *ContainerTypeName);
		return false;
	}

	static bool ParseReplicationCondition(const FString& ConditionName, ELifetimeCondition& OutCondition, FString& OutError)
	{
		const FString Normalized = ConditionName.TrimStartAndEnd().ToLower();

		if (Normalized.IsEmpty() || Normalized == TEXT("none") || Normalized == TEXT("cond_none"))
		{
			OutCondition = COND_None;
			return true;
		}
		if (Normalized == TEXT("initial_only") || Normalized == TEXT("initialonly") || Normalized == TEXT("cond_initialonly"))
		{
			OutCondition = COND_InitialOnly;
			return true;
		}
		if (Normalized == TEXT("owner_only") || Normalized == TEXT("owneronly") || Normalized == TEXT("cond_owneronly"))
		{
			OutCondition = COND_OwnerOnly;
			return true;
		}
		if (Normalized == TEXT("skip_owner") || Normalized == TEXT("skipowner") || Normalized == TEXT("cond_skipowner"))
		{
			OutCondition = COND_SkipOwner;
			return true;
		}
		if (Normalized == TEXT("simulated_only") || Normalized == TEXT("simulatedonly") || Normalized == TEXT("cond_simulatedonly"))
		{
			OutCondition = COND_SimulatedOnly;
			return true;
		}
		if (Normalized == TEXT("autonomous_only") || Normalized == TEXT("autonomousonly") || Normalized == TEXT("cond_autonomousonly"))
		{
			OutCondition = COND_AutonomousOnly;
			return true;
		}
		if (Normalized == TEXT("simulated_or_physics") || Normalized == TEXT("simulatedorphysics") || Normalized == TEXT("cond_simulatedorphysics"))
		{
			OutCondition = COND_SimulatedOrPhysics;
			return true;
		}
		if (Normalized == TEXT("initial_or_owner") || Normalized == TEXT("initialorowner") || Normalized == TEXT("cond_initialorowner"))
		{
			OutCondition = COND_InitialOrOwner;
			return true;
		}
		if (Normalized == TEXT("custom") || Normalized == TEXT("cond_custom"))
		{
			OutCondition = COND_Custom;
			return true;
		}
		if (Normalized == TEXT("replay_or_owner") || Normalized == TEXT("replayorowner") || Normalized == TEXT("cond_replayorowner"))
		{
			OutCondition = COND_ReplayOrOwner;
			return true;
		}
		if (Normalized == TEXT("replay_only") || Normalized == TEXT("replayonly") || Normalized == TEXT("cond_replayonly"))
		{
			OutCondition = COND_ReplayOnly;
			return true;
		}
		if (Normalized == TEXT("simulated_only_no_replay") || Normalized == TEXT("simulatedonlynoreplay") || Normalized == TEXT("cond_simulatedonlynoreplay"))
		{
			OutCondition = COND_SimulatedOnlyNoReplay;
			return true;
		}
		if (Normalized == TEXT("simulated_or_physics_no_replay") || Normalized == TEXT("simulatedorphysicsnoreplay") || Normalized == TEXT("cond_simulatedorphysicsnoreplay"))
		{
			OutCondition = COND_SimulatedOrPhysicsNoReplay;
			return true;
		}
		if (Normalized == TEXT("skip_replay") || Normalized == TEXT("skipreplay") || Normalized == TEXT("cond_skipreplay"))
		{
			OutCondition = COND_SkipReplay;
			return true;
		}
		if (Normalized == TEXT("dynamic") || Normalized == TEXT("cond_dynamic"))
		{
			OutCondition = COND_Dynamic;
			return true;
		}
		if (Normalized == TEXT("never") || Normalized == TEXT("cond_never"))
		{
			OutCondition = COND_Never;
			return true;
		}
		if (Normalized == TEXT("net_group") || Normalized == TEXT("netgroup") || Normalized == TEXT("cond_netgroup"))
		{
			OutCondition = COND_NetGroup;
			return true;
		}

		OutError = FString::Printf(
			TEXT("Unsupported replication_condition '%s'. Supported: none, initial_only, owner_only, skip_owner, simulated_only, autonomous_only, simulated_or_physics, initial_or_owner, custom, replay_or_owner, replay_only, simulated_only_no_replay, simulated_or_physics_no_replay, skip_replay, dynamic, never, net_group"),
			*ConditionName
		);
		return false;
	}

	static FString ReplicationConditionToString(const ELifetimeCondition Condition)
	{
		switch (Condition)
		{
			case COND_None:
				return TEXT("none");
			case COND_InitialOnly:
				return TEXT("initial_only");
			case COND_OwnerOnly:
				return TEXT("owner_only");
			case COND_SkipOwner:
				return TEXT("skip_owner");
			case COND_SimulatedOnly:
				return TEXT("simulated_only");
			case COND_AutonomousOnly:
				return TEXT("autonomous_only");
			case COND_SimulatedOrPhysics:
				return TEXT("simulated_or_physics");
			case COND_InitialOrOwner:
				return TEXT("initial_or_owner");
			case COND_Custom:
				return TEXT("custom");
			case COND_ReplayOrOwner:
				return TEXT("replay_or_owner");
			case COND_ReplayOnly:
				return TEXT("replay_only");
			case COND_SimulatedOnlyNoReplay:
				return TEXT("simulated_only_no_replay");
			case COND_SimulatedOrPhysicsNoReplay:
				return TEXT("simulated_or_physics_no_replay");
			case COND_SkipReplay:
				return TEXT("skip_replay");
			case COND_Dynamic:
				return TEXT("dynamic");
			case COND_Never:
				return TEXT("never");
			case COND_NetGroup:
				return TEXT("net_group");
			default:
				return TEXT("none");
		}
	}

	static bool SplitMapTypeArguments(const FString& Arguments, FString& OutKeyType, FString& OutValueType)
	{
		int32 Depth = 0;
		for (int32 Index = 0; Index < Arguments.Len(); ++Index)
		{
			const TCHAR Character = Arguments[Index];
			if (Character == TCHAR('<'))
			{
				++Depth;
			}
			else if (Character == TCHAR('>'))
			{
				Depth = FMath::Max(0, Depth - 1);
			}
			else if (Character == TCHAR(',') && Depth == 0)
			{
				OutKeyType = Arguments.Left(Index).TrimStartAndEnd();
				OutValueType = Arguments.Mid(Index + 1).TrimStartAndEnd();
				return !OutKeyType.IsEmpty() && !OutValueType.IsEmpty();
			}
		}
		return false;
	}

	static bool ParseContainerFromVariableType(
		const FString& VariableTypeName,
		FString& OutValueTypeName,
		EPinContainerType& OutContainerType,
		FString& OutMapKeyTypeName,
		FString& OutError
	)
	{
		const FString TrimmedType = VariableTypeName.TrimStartAndEnd();
		const FString LowerType = TrimmedType.ToLower();

		OutContainerType = EPinContainerType::None;
		OutValueTypeName = TrimmedType;
		OutMapKeyTypeName.Reset();

		auto ParseSingleArgContainer = [&](const TCHAR* Prefix, const EPinContainerType ContainerType) -> bool
		{
			const FString PrefixString(Prefix);
			if (!LowerType.StartsWith(PrefixString))
			{
				return false;
			}
			if (!TrimmedType.EndsWith(TEXT(">")))
			{
				OutError = FString::Printf(TEXT("Malformed variable_type '%s'"), *VariableTypeName);
				return true;
			}

			const int32 PrefixLength = PrefixString.Len();
			const FString InnerType = TrimmedType.Mid(PrefixLength, TrimmedType.Len() - PrefixLength - 1).TrimStartAndEnd();
			if (InnerType.IsEmpty())
			{
				OutError = FString::Printf(TEXT("Malformed variable_type '%s'"), *VariableTypeName);
				return true;
			}

			OutContainerType = ContainerType;
			OutValueTypeName = InnerType;
			return true;
		};

		if (ParseSingleArgContainer(TEXT("array<"), EPinContainerType::Array))
		{
			return OutError.IsEmpty();
		}
		if (ParseSingleArgContainer(TEXT("set<"), EPinContainerType::Set))
		{
			return OutError.IsEmpty();
		}

		if (LowerType.StartsWith(TEXT("map<")))
		{
			if (!TrimmedType.EndsWith(TEXT(">")))
			{
				OutError = FString::Printf(TEXT("Malformed variable_type '%s'"), *VariableTypeName);
				return false;
			}

			const FString InnerTypes = TrimmedType.Mid(4, TrimmedType.Len() - 5).TrimStartAndEnd();
			FString KeyTypeName;
			FString ValueTypeName;
			if (!SplitMapTypeArguments(InnerTypes, KeyTypeName, ValueTypeName))
			{
				OutError = FString::Printf(TEXT("Malformed map variable_type '%s'. Expected map<key_type, value_type>"), *VariableTypeName);
				return false;
			}

			OutContainerType = EPinContainerType::Map;
			OutMapKeyTypeName = KeyTypeName;
			OutValueTypeName = ValueTypeName;
			return true;
		}

		return true;
	}

	static bool IsTruthyMetadataValue(const FString& MetadataValue)
	{
		const FString Normalized = MetadataValue.TrimStartAndEnd().ToLower();
		return Normalized.IsEmpty() || Normalized == TEXT("true") || Normalized == TEXT("1") || Normalized == TEXT("yes");
	}

	static FString BaseTypeNameFromTypeData(const FName PinCategory, const FName PinSubCategory, const UObject* PinSubCategoryObject)
	{
		if (PinCategory == UEdGraphSchema_K2::PC_Boolean)
		{
			return TEXT("bool");
		}
		if (PinCategory == UEdGraphSchema_K2::PC_Int)
		{
			return TEXT("int");
		}
		if (PinCategory == UEdGraphSchema_K2::PC_Int64)
		{
			return TEXT("int64");
		}
		if (PinCategory == UEdGraphSchema_K2::PC_Real)
		{
			if (PinSubCategory == UEdGraphSchema_K2::PC_Double)
			{
				return TEXT("double");
			}
			return TEXT("float");
		}
		if (PinCategory == UEdGraphSchema_K2::PC_Name)
		{
			return TEXT("name");
		}
		if (PinCategory == UEdGraphSchema_K2::PC_String)
		{
			return TEXT("string");
		}
		if (PinCategory == UEdGraphSchema_K2::PC_Text)
		{
			return TEXT("text");
		}
		if (PinCategory == UEdGraphSchema_K2::PC_Object)
		{
			return TEXT("object");
		}
		if (PinCategory == UEdGraphSchema_K2::PC_Class)
		{
			return TEXT("class");
		}
		if (PinCategory == UEdGraphSchema_K2::PC_SoftObject)
		{
			return TEXT("soft_object");
		}
		if (PinCategory == UEdGraphSchema_K2::PC_SoftClass)
		{
			return TEXT("soft_class");
		}
		if (PinCategory == UEdGraphSchema_K2::PC_Interface)
		{
			return TEXT("interface");
		}
		if (PinCategory == UEdGraphSchema_K2::PC_Enum)
		{
			return TEXT("enum");
		}
		if (PinCategory == UEdGraphSchema_K2::PC_Byte)
		{
			if (PinSubCategoryObject && PinSubCategoryObject->IsA<UEnum>())
			{
				return TEXT("enum");
			}
			return TEXT("byte");
		}
		if (PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			if (PinSubCategoryObject == TBaseStructure<FVector>::Get())
			{
				return TEXT("vector");
			}
			if (PinSubCategoryObject == TBaseStructure<FRotator>::Get())
			{
				return TEXT("rotator");
			}
			if (PinSubCategoryObject == TBaseStructure<FTransform>::Get())
			{
				return TEXT("transform");
			}
			return TEXT("struct");
		}

		return PinCategory.ToString();
	}

	static FString BaseTypeNameFromPinType(const FEdGraphPinType& PinType)
	{
		return BaseTypeNameFromTypeData(PinType.PinCategory, PinType.PinSubCategory, PinType.PinSubCategoryObject.Get());
	}

	static FString BaseTypeNameFromTerminalType(const FEdGraphTerminalType& TerminalType)
	{
		return BaseTypeNameFromTypeData(TerminalType.TerminalCategory, TerminalType.TerminalSubCategory, TerminalType.TerminalSubCategoryObject.Get());
	}

	static bool BuildPinType(const FString& TypeName, const FString& TypeReferencePath, FEdGraphPinType& OutType, FString& OutError)
	{
		const FString NormalizedType = TypeName.TrimStartAndEnd().ToLower();
		const FString TrimmedReferencePath = TypeReferencePath.TrimStartAndEnd();

		OutType = FEdGraphPinType();
		OutType.ContainerType = EPinContainerType::None;
		OutType.PinValueType = FEdGraphTerminalType();

		if (NormalizedType == TEXT("bool") || NormalizedType == TEXT("boolean"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
			return true;
		}
		if (NormalizedType == TEXT("int") || NormalizedType == TEXT("int32"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Int;
			return true;
		}
		if (NormalizedType == TEXT("int64"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Int64;
			return true;
		}
		if (NormalizedType == TEXT("float"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Real;
			OutType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
			return true;
		}
		if (NormalizedType == TEXT("double"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Real;
			OutType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
			return true;
		}
		if (NormalizedType == TEXT("name"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Name;
			return true;
		}
		if (NormalizedType == TEXT("string"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_String;
			return true;
		}
		if (NormalizedType == TEXT("text"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Text;
			return true;
		}
		if (NormalizedType == TEXT("byte"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Byte;
			if (!TrimmedReferencePath.IsEmpty())
			{
				UEnum* EnumObject = ResolveObjectByNameOrPath<UEnum>(TrimmedReferencePath);
				if (!EnumObject)
				{
					OutError = FString::Printf(TEXT("Enum not found for byte type_reference: %s"), *TrimmedReferencePath);
					return false;
				}
				OutType.PinSubCategoryObject = EnumObject;
			}
			return true;
		}
		if (NormalizedType == TEXT("vector"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
			return true;
		}
		if (NormalizedType == TEXT("rotator"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
			return true;
		}
		if (NormalizedType == TEXT("transform"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
			return true;
		}
		if (NormalizedType == TEXT("object") || NormalizedType == TEXT("object_reference"))
		{
			UClass* ObjectClass = TrimmedReferencePath.IsEmpty() ? UObject::StaticClass() : ResolveClassByNameOrPath(TrimmedReferencePath);
			if (!ObjectClass)
			{
				OutError = FString::Printf(TEXT("Class not found for object type_reference: %s"), *TrimmedReferencePath);
				return false;
			}
			OutType.PinCategory = UEdGraphSchema_K2::PC_Object;
			OutType.PinSubCategoryObject = ObjectClass;
			return true;
		}
		if (NormalizedType == TEXT("class") || NormalizedType == TEXT("class_reference"))
		{
			UClass* MetaClass = TrimmedReferencePath.IsEmpty() ? UObject::StaticClass() : ResolveClassByNameOrPath(TrimmedReferencePath);
			if (!MetaClass)
			{
				OutError = FString::Printf(TEXT("Class not found for class type_reference: %s"), *TrimmedReferencePath);
				return false;
			}
			OutType.PinCategory = UEdGraphSchema_K2::PC_Class;
			OutType.PinSubCategoryObject = MetaClass;
			return true;
		}
		if (NormalizedType == TEXT("soft_object") || NormalizedType == TEXT("softobject") || NormalizedType == TEXT("soft_object_reference"))
		{
			UClass* ObjectClass = TrimmedReferencePath.IsEmpty() ? UObject::StaticClass() : ResolveClassByNameOrPath(TrimmedReferencePath);
			if (!ObjectClass)
			{
				OutError = FString::Printf(TEXT("Class not found for soft_object type_reference: %s"), *TrimmedReferencePath);
				return false;
			}
			OutType.PinCategory = UEdGraphSchema_K2::PC_SoftObject;
			OutType.PinSubCategoryObject = ObjectClass;
			return true;
		}
		if (NormalizedType == TEXT("soft_class") || NormalizedType == TEXT("softclass") || NormalizedType == TEXT("soft_class_reference"))
		{
			UClass* MetaClass = TrimmedReferencePath.IsEmpty() ? UObject::StaticClass() : ResolveClassByNameOrPath(TrimmedReferencePath);
			if (!MetaClass)
			{
				OutError = FString::Printf(TEXT("Class not found for soft_class type_reference: %s"), *TrimmedReferencePath);
				return false;
			}
			OutType.PinCategory = UEdGraphSchema_K2::PC_SoftClass;
			OutType.PinSubCategoryObject = MetaClass;
			return true;
		}
		if (NormalizedType == TEXT("interface"))
		{
			if (TrimmedReferencePath.IsEmpty())
			{
				OutError = TEXT("interface variable_type requires type_reference to an interface class path");
				return false;
			}

			UClass* InterfaceClass = ResolveClassByNameOrPath(TrimmedReferencePath);
			if (!InterfaceClass)
			{
				OutError = FString::Printf(TEXT("Interface class not found: %s"), *TrimmedReferencePath);
				return false;
			}
			if (!InterfaceClass->HasAnyClassFlags(CLASS_Interface) && !InterfaceClass->IsChildOf(UInterface::StaticClass()))
			{
				OutError = FString::Printf(TEXT("Class is not an interface: %s"), *InterfaceClass->GetPathName());
				return false;
			}

			OutType.PinCategory = UEdGraphSchema_K2::PC_Interface;
			OutType.PinSubCategoryObject = InterfaceClass;
			return true;
		}
		if (NormalizedType == TEXT("enum"))
		{
			if (TrimmedReferencePath.IsEmpty())
			{
				OutError = TEXT("enum variable_type requires type_reference to a UEnum path");
				return false;
			}

			UEnum* EnumObject = ResolveObjectByNameOrPath<UEnum>(TrimmedReferencePath);
			if (!EnumObject)
			{
				OutError = FString::Printf(TEXT("Enum not found: %s"), *TrimmedReferencePath);
				return false;
			}

			OutType.PinCategory = UEdGraphSchema_K2::PC_Enum;
			OutType.PinSubCategoryObject = EnumObject;
			return true;
		}
		if (NormalizedType == TEXT("struct"))
		{
			if (TrimmedReferencePath.IsEmpty())
			{
				OutError = TEXT("struct variable_type requires type_reference to a UScriptStruct path");
				return false;
			}

			UScriptStruct* ScriptStruct = ResolveObjectByNameOrPath<UScriptStruct>(TrimmedReferencePath);
			if (!ScriptStruct)
			{
				OutError = FString::Printf(TEXT("Struct not found: %s"), *TrimmedReferencePath);
				return false;
			}

			OutType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutType.PinSubCategoryObject = ScriptStruct;
			return true;
		}

		OutError = FString::Printf(
			TEXT("Unsupported variable_type '%s'. Supported: bool, int, int64, float, double, name, string, text, byte, vector, rotator, transform, object, class, soft_object, soft_class, interface, enum, struct"),
			*TypeName
		);
		return false;
	}

	static bool BuildPinType(const FString& TypeName, FEdGraphPinType& OutType, FString& OutError)
	{
		return BuildPinType(TypeName, FString(), OutType, OutError);
	}

	static bool ParseBlueprintType(const FString& TypeName, EBlueprintType& OutType, FString& OutError)
	{
		const FString Normalized = TypeName.TrimStartAndEnd().ToLower();

		if (Normalized.IsEmpty() || Normalized == TEXT("normal"))
		{
			OutType = BPTYPE_Normal;
			return true;
		}
		if (Normalized == TEXT("const"))
		{
			OutType = BPTYPE_Const;
			return true;
		}
		if (Normalized == TEXT("macro") || Normalized == TEXT("macro_library"))
		{
			OutType = BPTYPE_MacroLibrary;
			return true;
		}
		if (Normalized == TEXT("interface"))
		{
			OutType = BPTYPE_Interface;
			return true;
		}
		if (Normalized == TEXT("level_script"))
		{
			OutType = BPTYPE_LevelScript;
			return true;
		}
		if (Normalized == TEXT("function_library"))
		{
			OutType = BPTYPE_FunctionLibrary;
			return true;
		}

		OutError = FString::Printf(
			TEXT("Unsupported blueprint_type '%s'. Supported: normal, const, macro_library, interface, level_script, function_library"),
			*TypeName
		);
		return false;
	}

	static FString BlueprintTypeToString(const EBlueprintType BlueprintType)
	{
		switch (BlueprintType)
		{
			case BPTYPE_Normal:
				return TEXT("normal");
			case BPTYPE_Const:
				return TEXT("const");
			case BPTYPE_MacroLibrary:
				return TEXT("macro_library");
			case BPTYPE_Interface:
				return TEXT("interface");
			case BPTYPE_LevelScript:
				return TEXT("level_script");
			case BPTYPE_FunctionLibrary:
				return TEXT("function_library");
			default:
				return TEXT("unknown");
		}
	}

	static FString BlueprintStatusToString(const EBlueprintStatus Status)
	{
		switch (Status)
		{
			case BS_Unknown:
				return TEXT("unknown");
			case BS_Dirty:
				return TEXT("dirty");
			case BS_Error:
				return TEXT("error");
			case BS_UpToDate:
				return TEXT("up_to_date");
			case BS_BeingCreated:
				return TEXT("being_created");
			case BS_UpToDateWithWarnings:
				return TEXT("up_to_date_with_warnings");
			default:
				return TEXT("unknown");
		}
	}

	static bool ParseAutoReceiveInput(const FString& InputMode, EAutoReceiveInput::Type& OutInput)
	{
		const FString Normalized = InputMode.TrimStartAndEnd().ToLower();

		if (Normalized == TEXT("disabled") || Normalized == TEXT("none") || Normalized == TEXT("0"))
		{
			OutInput = EAutoReceiveInput::Disabled;
			return true;
		}
		if (Normalized == TEXT("player0") || Normalized == TEXT("player_0") || Normalized == TEXT("1"))
		{
			OutInput = EAutoReceiveInput::Player0;
			return true;
		}
		if (Normalized == TEXT("player1") || Normalized == TEXT("player_1") || Normalized == TEXT("2"))
		{
			OutInput = EAutoReceiveInput::Player1;
			return true;
		}
		if (Normalized == TEXT("player2") || Normalized == TEXT("player_2") || Normalized == TEXT("3"))
		{
			OutInput = EAutoReceiveInput::Player2;
			return true;
		}
		if (Normalized == TEXT("player3") || Normalized == TEXT("player_3") || Normalized == TEXT("4"))
		{
			OutInput = EAutoReceiveInput::Player3;
			return true;
		}
		if (Normalized == TEXT("player4") || Normalized == TEXT("player_4") || Normalized == TEXT("5"))
		{
			OutInput = EAutoReceiveInput::Player4;
			return true;
		}
		if (Normalized == TEXT("player5") || Normalized == TEXT("player_5") || Normalized == TEXT("6"))
		{
			OutInput = EAutoReceiveInput::Player5;
			return true;
		}
		if (Normalized == TEXT("player6") || Normalized == TEXT("player_6") || Normalized == TEXT("7"))
		{
			OutInput = EAutoReceiveInput::Player6;
			return true;
		}
		if (Normalized == TEXT("player7") || Normalized == TEXT("player_7") || Normalized == TEXT("8"))
		{
			OutInput = EAutoReceiveInput::Player7;
			return true;
		}

		return false;
	}

	static FString AutoReceiveInputToString(const EAutoReceiveInput::Type InputMode)
	{
		switch (InputMode)
		{
			case EAutoReceiveInput::Disabled:
				return TEXT("disabled");
			case EAutoReceiveInput::Player0:
				return TEXT("player0");
			case EAutoReceiveInput::Player1:
				return TEXT("player1");
			case EAutoReceiveInput::Player2:
				return TEXT("player2");
			case EAutoReceiveInput::Player3:
				return TEXT("player3");
			case EAutoReceiveInput::Player4:
				return TEXT("player4");
			case EAutoReceiveInput::Player5:
				return TEXT("player5");
			case EAutoReceiveInput::Player6:
				return TEXT("player6");
			case EAutoReceiveInput::Player7:
				return TEXT("player7");
			default:
				return TEXT("disabled");
		}
	}

	static void SetClassFlag(UClass* Class, const EClassFlags Flag, const bool bEnabled)
	{
		if (!Class)
		{
			return;
		}

		if (bEnabled)
		{
			Class->ClassFlags |= Flag;
		}
		else
		{
			Class->ClassFlags &= ~Flag;
		}
	}

	enum class EBlueprintGraphType : uint8
	{
		Unknown,
		EventGraph,
		Function,
		Macro,
		DelegateSignature,
		Interface
	};

	enum class ECreateGraphType : uint8
	{
		Function,
		Macro,
		Event
	};

	static FString GraphTypeToString(const EBlueprintGraphType GraphType)
	{
		switch (GraphType)
		{
			case EBlueprintGraphType::EventGraph:
				return TEXT("event_graph");
			case EBlueprintGraphType::Function:
				return TEXT("function");
			case EBlueprintGraphType::Macro:
				return TEXT("macro");
			case EBlueprintGraphType::DelegateSignature:
				return TEXT("delegate_signature");
			case EBlueprintGraphType::Interface:
				return TEXT("interface");
			default:
				return TEXT("unknown");
		}
	}

	static FString AccessSpecifierToString(const uint32 AccessSpecifier)
	{
		switch (AccessSpecifier & FUNC_AccessSpecifiers)
		{
			case FUNC_Public:
				return TEXT("public");
			case FUNC_Protected:
				return TEXT("protected");
			case FUNC_Private:
				return TEXT("private");
			default:
				return TEXT("public");
		}
	}

	static bool ParseGraphType(const FString& GraphTypeName, ECreateGraphType& OutGraphType, FString& OutError)
	{
		const FString Normalized = GraphTypeName.TrimStartAndEnd().ToLower();

		if (Normalized.IsEmpty() || Normalized == TEXT("function"))
		{
			OutGraphType = ECreateGraphType::Function;
			return true;
		}
		if (Normalized == TEXT("macro"))
		{
			OutGraphType = ECreateGraphType::Macro;
			return true;
		}
		if (Normalized == TEXT("event") || Normalized == TEXT("event_graph") || Normalized == TEXT("ubergraph"))
		{
			OutGraphType = ECreateGraphType::Event;
			return true;
		}

		OutError = FString::Printf(
			TEXT("Unsupported graph_type '%s'. Supported: function, macro, event_graph"),
			*GraphTypeName
		);
		return false;
	}

	static bool ParseAccessSpecifier(const FString& AccessSpecifierName, uint32& OutAccessSpecifier, FString& OutError)
	{
		const FString Normalized = AccessSpecifierName.TrimStartAndEnd().ToLower();

		if (Normalized == TEXT("public"))
		{
			OutAccessSpecifier = FUNC_Public;
			return true;
		}
		if (Normalized == TEXT("protected"))
		{
			OutAccessSpecifier = FUNC_Protected;
			return true;
		}
		if (Normalized == TEXT("private"))
		{
			OutAccessSpecifier = FUNC_Private;
			return true;
		}

		OutError = FString::Printf(
			TEXT("Unsupported access '%s'. Supported: public, protected, private"),
			*AccessSpecifierName
		);
		return false;
	}

	static EBlueprintGraphType GetBlueprintGraphType(const UBlueprint* Blueprint, const UEdGraph* Graph)
	{
		if (!Blueprint || !Graph)
		{
			return EBlueprintGraphType::Unknown;
		}

		if (Blueprint->UbergraphPages.Contains(Graph))
		{
			return EBlueprintGraphType::EventGraph;
		}
		if (Blueprint->FunctionGraphs.Contains(Graph))
		{
			return EBlueprintGraphType::Function;
		}
		if (Blueprint->MacroGraphs.Contains(Graph))
		{
			return EBlueprintGraphType::Macro;
		}
		if (Blueprint->DelegateSignatureGraphs.Contains(Graph))
		{
			return EBlueprintGraphType::DelegateSignature;
		}

		for (const FBPInterfaceDescription& InterfaceDescription : Blueprint->ImplementedInterfaces)
		{
			if (InterfaceDescription.Graphs.Contains(Graph))
			{
				return EBlueprintGraphType::Interface;
			}
		}

		return EBlueprintGraphType::Unknown;
	}

	static TSharedPtr<FJsonObject> BuildGraphJson(const UBlueprint* Blueprint, const UEdGraph* Graph)
	{
		TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
		GraphObj->SetStringField(TEXT("graph_name"), Graph ? Graph->GetName() : TEXT("None"));
		GraphObj->SetStringField(TEXT("graph_type"), GraphTypeToString(GetBlueprintGraphType(Blueprint, Graph)));
		GraphObj->SetStringField(TEXT("graph_path"), Graph ? Graph->GetPathName() : TEXT("None"));
		GraphObj->SetNumberField(TEXT("node_count"), Graph ? Graph->Nodes.Num() : 0);
		GraphObj->SetBoolField(TEXT("is_read_only"), Graph ? FBlueprintEditorUtils::IsGraphReadOnly(const_cast<UEdGraph*>(Graph)) : true);

		if (Graph && Graph->GetSchema())
		{
			GraphObj->SetStringField(TEXT("schema_class"), Graph->GetSchema()->GetClass()->GetName());
		}

		if (Graph)
		{
			if (FKismetUserDeclaredFunctionMetadata* Metadata = FBlueprintEditorUtils::GetGraphFunctionMetaData(Graph))
			{
				GraphObj->SetStringField(TEXT("category"), Metadata->Category.ToString());
				GraphObj->SetStringField(TEXT("tooltip"), Metadata->ToolTip.ToString());
			}

			if (const UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(FBlueprintEditorUtils::GetEntryNode(Graph)))
			{
				const uint32 AccessSpecifier = EntryNode->GetFunctionFlags() & FUNC_AccessSpecifiers;
				GraphObj->SetStringField(TEXT("access"), AccessSpecifierToString(AccessSpecifier));
			}
		}

		return GraphObj;
	}

	static TSharedPtr<FJsonObject> BuildVariableJson(const UBlueprint* Blueprint, const FBPVariableDescription& Variable)
	{
		TSharedPtr<FJsonObject> VariableObj = MakeShared<FJsonObject>();
		const FEdGraphPinType& PinType = Variable.VarType;
		const uint64 PropertyFlags = Variable.PropertyFlags;
		const FString BaseTypeName = BaseTypeNameFromPinType(PinType);

		VariableObj->SetStringField(TEXT("variable_name"), Variable.VarName.ToString());
		VariableObj->SetStringField(TEXT("friendly_name"), Variable.FriendlyName);
		VariableObj->SetStringField(TEXT("guid"), Variable.VarGuid.ToString(EGuidFormats::DigitsWithHyphens));
		VariableObj->SetStringField(TEXT("base_type"), BaseTypeName);
		VariableObj->SetStringField(TEXT("container_type"), PinContainerTypeToString(PinType.ContainerType));
		VariableObj->SetStringField(TEXT("category"), Variable.Category.ToString());
		VariableObj->SetStringField(TEXT("default_value"), Variable.DefaultValue);
		VariableObj->SetNumberField(TEXT("property_flags"), static_cast<double>(PropertyFlags));

		if (PinType.ContainerType == EPinContainerType::Map)
		{
			const FString KeyTypeName = BaseTypeName;
			const FString ValueTypeName = BaseTypeNameFromTerminalType(PinType.PinValueType);
			VariableObj->SetStringField(TEXT("map_key_type"), KeyTypeName);
			VariableObj->SetStringField(TEXT("map_value_type"), ValueTypeName);
			VariableObj->SetStringField(TEXT("variable_type"), FString::Printf(TEXT("map<%s,%s>"), *KeyTypeName, *ValueTypeName));

			if (PinType.PinSubCategoryObject.IsValid())
			{
				VariableObj->SetStringField(TEXT("map_key_type_reference"), PinType.PinSubCategoryObject->GetPathName());
			}
			if (PinType.PinValueType.TerminalSubCategoryObject.IsValid())
			{
				VariableObj->SetStringField(TEXT("map_value_type_reference"), PinType.PinValueType.TerminalSubCategoryObject->GetPathName());
			}
		}
		else if (PinType.ContainerType == EPinContainerType::Array)
		{
			VariableObj->SetStringField(TEXT("variable_type"), FString::Printf(TEXT("array<%s>"), *BaseTypeName));
		}
		else if (PinType.ContainerType == EPinContainerType::Set)
		{
			VariableObj->SetStringField(TEXT("variable_type"), FString::Printf(TEXT("set<%s>"), *BaseTypeName));
		}
		else
		{
			VariableObj->SetStringField(TEXT("variable_type"), BaseTypeName);
		}

		if (PinType.PinSubCategoryObject.IsValid())
		{
			VariableObj->SetStringField(TEXT("type_reference"), PinType.PinSubCategoryObject->GetPathName());
		}

		FString TooltipMetadata;
		const bool bHasTooltipMetadata = FBlueprintEditorUtils::GetBlueprintVariableMetaData(Blueprint, Variable.VarName, nullptr, FBlueprintMetadata::MD_Tooltip, TooltipMetadata);
		VariableObj->SetStringField(TEXT("tooltip"), bHasTooltipMetadata ? TooltipMetadata : FString());

		FString PrivateMetadata;
		const bool bHasPrivateMetadata = FBlueprintEditorUtils::GetBlueprintVariableMetaData(Blueprint, Variable.VarName, nullptr, FBlueprintMetadata::MD_Private, PrivateMetadata);
		VariableObj->SetBoolField(TEXT("private"), bHasPrivateMetadata && IsTruthyMetadataValue(PrivateMetadata));

		FString ExposeOnSpawnMetadata;
		const bool bHasExposeOnSpawnMetadata = FBlueprintEditorUtils::GetBlueprintVariableMetaData(Blueprint, Variable.VarName, nullptr, FBlueprintMetadata::MD_ExposeOnSpawn, ExposeOnSpawnMetadata);
		VariableObj->SetBoolField(TEXT("expose_on_spawn"), bHasExposeOnSpawnMetadata && IsTruthyMetadataValue(ExposeOnSpawnMetadata));

		VariableObj->SetBoolField(TEXT("instance_editable"), (PropertyFlags & CPF_DisableEditOnInstance) == 0);
		VariableObj->SetBoolField(TEXT("save_game"), (PropertyFlags & CPF_SaveGame) != 0);
		VariableObj->SetBoolField(TEXT("transient"), (PropertyFlags & CPF_Transient) != 0);
		VariableObj->SetBoolField(TEXT("advanced_display"), (PropertyFlags & CPF_AdvancedDisplay) != 0);
		VariableObj->SetBoolField(TEXT("replicated"), (PropertyFlags & CPF_Net) != 0);
		VariableObj->SetBoolField(TEXT("rep_notify_enabled"), (PropertyFlags & CPF_RepNotify) != 0);
		VariableObj->SetStringField(TEXT("rep_notify_function"), Variable.RepNotifyFunc.IsNone() ? TEXT("") : Variable.RepNotifyFunc.ToString());
		VariableObj->SetStringField(TEXT("replication_condition"), ReplicationConditionToString(Variable.ReplicationCondition));
		return VariableObj;
	}
}

FBlueprintService::FBlueprintService()
{
}

FString FBlueprintService::GetServiceDescription() const
{
	return TEXT("Blueprint graph authoring - create variables, add nodes, connect pins, and compile");
}

TArray<FMCPToolInfo> FBlueprintService::GetAvailableTools() const
{
	TArray<FMCPToolInfo> Tools;

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("create_blueprint");
		Tool.Description = TEXT("Create a new Blueprint asset.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Target Blueprint asset path (e.g. /Game/Blueprints/BP_MyActor)."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> ParentParam = MakeShared<FJsonObject>();
		ParentParam->SetStringField(TEXT("type"), TEXT("string"));
		ParentParam->SetStringField(TEXT("description"), TEXT("Parent class path or class name (default: /Script/Engine.Actor)."));
		Tool.Parameters->SetObjectField(TEXT("parent_class"), ParentParam);

		TSharedPtr<FJsonObject> TypeParam = MakeShared<FJsonObject>();
		TypeParam->SetStringField(TEXT("type"), TEXT("string"));
		TypeParam->SetStringField(TEXT("description"), TEXT("Blueprint type: normal, const, macro_library, interface, level_script, function_library."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_type"), TypeParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("duplicate_blueprint");
		Tool.Description = TEXT("Duplicate a Blueprint asset to a new path.");

		TSharedPtr<FJsonObject> SourceParam = MakeShared<FJsonObject>();
		SourceParam->SetStringField(TEXT("type"), TEXT("string"));
		SourceParam->SetStringField(TEXT("description"), TEXT("Source Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("source_blueprint_path"), SourceParam);

		TSharedPtr<FJsonObject> DestParam = MakeShared<FJsonObject>();
		DestParam->SetStringField(TEXT("type"), TEXT("string"));
		DestParam->SetStringField(TEXT("description"), TEXT("Destination Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("destination_blueprint_path"), DestParam);

		Tool.RequiredParams.Add(TEXT("source_blueprint_path"));
		Tool.RequiredParams.Add(TEXT("destination_blueprint_path"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("rename_blueprint");
		Tool.Description = TEXT("Rename or move a Blueprint asset to a new path.");

		TSharedPtr<FJsonObject> SourceParam = MakeShared<FJsonObject>();
		SourceParam->SetStringField(TEXT("type"), TEXT("string"));
		SourceParam->SetStringField(TEXT("description"), TEXT("Current Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), SourceParam);

		TSharedPtr<FJsonObject> DestParam = MakeShared<FJsonObject>();
		DestParam->SetStringField(TEXT("type"), TEXT("string"));
		DestParam->SetStringField(TEXT("description"), TEXT("New Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("new_blueprint_path"), DestParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("new_blueprint_path"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("delete_blueprint");
		Tool.Description = TEXT("Delete a Blueprint asset.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("save_blueprint");
		Tool.Description = TEXT("Save a Blueprint asset to disk.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> DirtyParam = MakeShared<FJsonObject>();
		DirtyParam->SetStringField(TEXT("type"), TEXT("boolean"));
		DirtyParam->SetStringField(TEXT("description"), TEXT("Only save if dirty (default: true)."));
		Tool.Parameters->SetObjectField(TEXT("only_if_dirty"), DirtyParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("reparent_blueprint");
		Tool.Description = TEXT("Reparent a Blueprint to a new parent class.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> ParentParam = MakeShared<FJsonObject>();
		ParentParam->SetStringField(TEXT("type"), TEXT("string"));
		ParentParam->SetStringField(TEXT("description"), TEXT("New parent class path or name."));
		Tool.Parameters->SetObjectField(TEXT("parent_class"), ParentParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("parent_class"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("get_blueprint_info");
		Tool.Description = TEXT("Get Blueprint asset and class metadata.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("set_class_settings");
		Tool.Description = TEXT("Set Blueprint class settings for tick, replication, input, and class/config flags.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> TickParam = MakeShared<FJsonObject>();
		TickParam->SetStringField(TEXT("type"), TEXT("object"));
		TickParam->SetStringField(TEXT("description"), TEXT("Tick settings: enabled, start_enabled, interval."));
		Tool.Parameters->SetObjectField(TEXT("tick"), TickParam);

		TSharedPtr<FJsonObject> ReplicationParam = MakeShared<FJsonObject>();
		ReplicationParam->SetStringField(TEXT("type"), TEXT("object"));
		ReplicationParam->SetStringField(TEXT("description"), TEXT("Replication settings: replicates, replicate_movement, net_load_on_client, always_relevant, only_relevant_to_owner, use_owner_relevancy, net_update_frequency, min_net_update_frequency, net_cull_distance_squared."));
		Tool.Parameters->SetObjectField(TEXT("replication"), ReplicationParam);

		TSharedPtr<FJsonObject> InputParam = MakeShared<FJsonObject>();
		InputParam->SetStringField(TEXT("type"), TEXT("object"));
		InputParam->SetStringField(TEXT("description"), TEXT("Input settings: auto_receive_input (disabled/player0..player7 or 0..8), input_priority, block_input."));
		Tool.Parameters->SetObjectField(TEXT("input"), InputParam);

		TSharedPtr<FJsonObject> FlagsParam = MakeShared<FJsonObject>();
		FlagsParam->SetStringField(TEXT("type"), TEXT("object"));
		FlagsParam->SetStringField(TEXT("description"), TEXT("Class/config flags: abstract, const_class, deprecated, run_construction_script_on_drag, config, default_config, config_do_not_check_defaults, not_placeable."));
		Tool.Parameters->SetObjectField(TEXT("class_flags"), FlagsParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("list_graphs");
		Tool.Description = TEXT("List top-level graphs in a Blueprint (event/function/macro/delegate/interface).");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("create_graph");
		Tool.Description = TEXT("Create a top-level Blueprint graph.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> GraphNameParam = MakeShared<FJsonObject>();
		GraphNameParam->SetStringField(TEXT("type"), TEXT("string"));
		GraphNameParam->SetStringField(TEXT("description"), TEXT("Graph name to create."));
		Tool.Parameters->SetObjectField(TEXT("graph_name"), GraphNameParam);

		TSharedPtr<FJsonObject> GraphTypeParam = MakeShared<FJsonObject>();
		GraphTypeParam->SetStringField(TEXT("type"), TEXT("string"));
		GraphTypeParam->SetStringField(TEXT("description"), TEXT("Graph type: function (default), macro, event_graph."));
		Tool.Parameters->SetObjectField(TEXT("graph_type"), GraphTypeParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("graph_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("rename_graph");
		Tool.Description = TEXT("Rename a Blueprint graph.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> GraphNameParam = MakeShared<FJsonObject>();
		GraphNameParam->SetStringField(TEXT("type"), TEXT("string"));
		GraphNameParam->SetStringField(TEXT("description"), TEXT("Existing graph name."));
		Tool.Parameters->SetObjectField(TEXT("graph_name"), GraphNameParam);

		TSharedPtr<FJsonObject> NewGraphNameParam = MakeShared<FJsonObject>();
		NewGraphNameParam->SetStringField(TEXT("type"), TEXT("string"));
		NewGraphNameParam->SetStringField(TEXT("description"), TEXT("New graph name."));
		Tool.Parameters->SetObjectField(TEXT("new_graph_name"), NewGraphNameParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("graph_name"));
		Tool.RequiredParams.Add(TEXT("new_graph_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("delete_graph");
		Tool.Description = TEXT("Delete a Blueprint graph.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> GraphNameParam = MakeShared<FJsonObject>();
		GraphNameParam->SetStringField(TEXT("type"), TEXT("string"));
		GraphNameParam->SetStringField(TEXT("description"), TEXT("Graph name to delete."));
		Tool.Parameters->SetObjectField(TEXT("graph_name"), GraphNameParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("graph_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("set_graph_metadata");
		Tool.Description = TEXT("Set graph metadata: category, tooltip, and function access (public/protected/private).");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> GraphNameParam = MakeShared<FJsonObject>();
		GraphNameParam->SetStringField(TEXT("type"), TEXT("string"));
		GraphNameParam->SetStringField(TEXT("description"), TEXT("Graph name."));
		Tool.Parameters->SetObjectField(TEXT("graph_name"), GraphNameParam);

		TSharedPtr<FJsonObject> CategoryParam = MakeShared<FJsonObject>();
		CategoryParam->SetStringField(TEXT("type"), TEXT("string"));
		CategoryParam->SetStringField(TEXT("description"), TEXT("Optional graph category."));
		Tool.Parameters->SetObjectField(TEXT("category"), CategoryParam);

		TSharedPtr<FJsonObject> TooltipParam = MakeShared<FJsonObject>();
		TooltipParam->SetStringField(TEXT("type"), TEXT("string"));
		TooltipParam->SetStringField(TEXT("description"), TEXT("Optional graph tooltip/description."));
		Tool.Parameters->SetObjectField(TEXT("tooltip"), TooltipParam);

		TSharedPtr<FJsonObject> AccessParam = MakeShared<FJsonObject>();
		AccessParam->SetStringField(TEXT("type"), TEXT("string"));
		AccessParam->SetStringField(TEXT("description"), TEXT("Optional access for function graphs: public, protected, private."));
		Tool.Parameters->SetObjectField(TEXT("access"), AccessParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("graph_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("format_graph");
		Tool.Description = TEXT("Auto-format graph nodes into a readable column layout.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> GraphNameParam = MakeShared<FJsonObject>();
		GraphNameParam->SetStringField(TEXT("type"), TEXT("string"));
		GraphNameParam->SetStringField(TEXT("description"), TEXT("Graph name."));
		Tool.Parameters->SetObjectField(TEXT("graph_name"), GraphNameParam);

		TSharedPtr<FJsonObject> StartXParam = MakeShared<FJsonObject>();
		StartXParam->SetStringField(TEXT("type"), TEXT("number"));
		StartXParam->SetStringField(TEXT("description"), TEXT("Optional start X position (default: 0)."));
		Tool.Parameters->SetObjectField(TEXT("start_x"), StartXParam);

		TSharedPtr<FJsonObject> StartYParam = MakeShared<FJsonObject>();
		StartYParam->SetStringField(TEXT("type"), TEXT("number"));
		StartYParam->SetStringField(TEXT("description"), TEXT("Optional start Y position (default: 0)."));
		Tool.Parameters->SetObjectField(TEXT("start_y"), StartYParam);

		TSharedPtr<FJsonObject> XSpacingParam = MakeShared<FJsonObject>();
		XSpacingParam->SetStringField(TEXT("type"), TEXT("number"));
		XSpacingParam->SetStringField(TEXT("description"), TEXT("Optional X spacing between columns (default: 420)."));
		Tool.Parameters->SetObjectField(TEXT("x_spacing"), XSpacingParam);

		TSharedPtr<FJsonObject> YSpacingParam = MakeShared<FJsonObject>();
		YSpacingParam->SetStringField(TEXT("type"), TEXT("number"));
		YSpacingParam->SetStringField(TEXT("description"), TEXT("Optional Y spacing between rows (default: 220)."));
		Tool.Parameters->SetObjectField(TEXT("y_spacing"), YSpacingParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("graph_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("list_graph_nodes");
		Tool.Description = TEXT("List graph nodes and pins for a Blueprint graph. Use returned node_id values with connect_pins.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path (e.g. /Game/Blueprints/BP_MyActor)."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> GraphParam = MakeShared<FJsonObject>();
		GraphParam->SetStringField(TEXT("type"), TEXT("string"));
		GraphParam->SetStringField(TEXT("description"), TEXT("Graph name (default: EventGraph)."));
		Tool.Parameters->SetObjectField(TEXT("graph_name"), GraphParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("create_variable");
		Tool.Description = TEXT("Create a Blueprint member variable.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> NameParam = MakeShared<FJsonObject>();
		NameParam->SetStringField(TEXT("type"), TEXT("string"));
		NameParam->SetStringField(TEXT("description"), TEXT("Variable name."));
		Tool.Parameters->SetObjectField(TEXT("variable_name"), NameParam);

		TSharedPtr<FJsonObject> TypeParam = MakeShared<FJsonObject>();
		TypeParam->SetStringField(TEXT("type"), TEXT("string"));
		TypeParam->SetStringField(TEXT("description"), TEXT("Variable type (e.g. bool, int, object, class, soft_object, soft_class, interface, enum, struct) or inline container syntax array<T>, set<T>, map<K,V>."));
		Tool.Parameters->SetObjectField(TEXT("variable_type"), TypeParam);

		TSharedPtr<FJsonObject> TypeReferenceParam = MakeShared<FJsonObject>();
		TypeReferenceParam->SetStringField(TEXT("type"), TEXT("string"));
		TypeReferenceParam->SetStringField(TEXT("description"), TEXT("Optional referenced type path for object/class/soft_object/soft_class/interface/enum/struct types."));
		Tool.Parameters->SetObjectField(TEXT("type_reference"), TypeReferenceParam);

		TSharedPtr<FJsonObject> ContainerTypeParam = MakeShared<FJsonObject>();
		ContainerTypeParam->SetStringField(TEXT("type"), TEXT("string"));
		ContainerTypeParam->SetStringField(TEXT("description"), TEXT("Optional container type override: none, array, set, map."));
		Tool.Parameters->SetObjectField(TEXT("container_type"), ContainerTypeParam);

		TSharedPtr<FJsonObject> MapKeyTypeParam = MakeShared<FJsonObject>();
		MapKeyTypeParam->SetStringField(TEXT("type"), TEXT("string"));
		MapKeyTypeParam->SetStringField(TEXT("description"), TEXT("Optional map key type when container_type=map (or use map<K,V> syntax in variable_type)."));
		Tool.Parameters->SetObjectField(TEXT("map_key_type"), MapKeyTypeParam);

		TSharedPtr<FJsonObject> MapKeyTypeReferenceParam = MakeShared<FJsonObject>();
		MapKeyTypeReferenceParam->SetStringField(TEXT("type"), TEXT("string"));
		MapKeyTypeReferenceParam->SetStringField(TEXT("description"), TEXT("Optional referenced type path for map key type."));
		Tool.Parameters->SetObjectField(TEXT("map_key_type_reference"), MapKeyTypeReferenceParam);

		TSharedPtr<FJsonObject> DefaultParam = MakeShared<FJsonObject>();
		DefaultParam->SetStringField(TEXT("type"), TEXT("string"));
		DefaultParam->SetStringField(TEXT("description"), TEXT("Optional default value string."));
		Tool.Parameters->SetObjectField(TEXT("default_value"), DefaultParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("variable_name"));
		Tool.RequiredParams.Add(TEXT("variable_type"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("list_variables");
		Tool.Description = TEXT("List Blueprint member variables and metadata.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("rename_variable");
		Tool.Description = TEXT("Rename a Blueprint member variable.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> VariableParam = MakeShared<FJsonObject>();
		VariableParam->SetStringField(TEXT("type"), TEXT("string"));
		VariableParam->SetStringField(TEXT("description"), TEXT("Existing variable name."));
		Tool.Parameters->SetObjectField(TEXT("variable_name"), VariableParam);

		TSharedPtr<FJsonObject> NewVariableParam = MakeShared<FJsonObject>();
		NewVariableParam->SetStringField(TEXT("type"), TEXT("string"));
		NewVariableParam->SetStringField(TEXT("description"), TEXT("New variable name."));
		Tool.Parameters->SetObjectField(TEXT("new_variable_name"), NewVariableParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("variable_name"));
		Tool.RequiredParams.Add(TEXT("new_variable_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("delete_variable");
		Tool.Description = TEXT("Delete a Blueprint member variable.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> VariableParam = MakeShared<FJsonObject>();
		VariableParam->SetStringField(TEXT("type"), TEXT("string"));
		VariableParam->SetStringField(TEXT("description"), TEXT("Variable name to remove."));
		Tool.Parameters->SetObjectField(TEXT("variable_name"), VariableParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("variable_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("set_variable_default");
		Tool.Description = TEXT("Set the default value string for a Blueprint member variable.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> VariableParam = MakeShared<FJsonObject>();
		VariableParam->SetStringField(TEXT("type"), TEXT("string"));
		VariableParam->SetStringField(TEXT("description"), TEXT("Variable name."));
		Tool.Parameters->SetObjectField(TEXT("variable_name"), VariableParam);

		TSharedPtr<FJsonObject> DefaultValueParam = MakeShared<FJsonObject>();
		DefaultValueParam->SetStringField(TEXT("type"), TEXT("string"));
		DefaultValueParam->SetStringField(TEXT("description"), TEXT("Variable default value string."));
		Tool.Parameters->SetObjectField(TEXT("default_value"), DefaultValueParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("variable_name"));
		Tool.RequiredParams.Add(TEXT("default_value"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("set_variable_metadata");
		Tool.Description = TEXT("Set variable metadata fields: category, tooltip, advanced_display, private.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> VariableParam = MakeShared<FJsonObject>();
		VariableParam->SetStringField(TEXT("type"), TEXT("string"));
		VariableParam->SetStringField(TEXT("description"), TEXT("Variable name."));
		Tool.Parameters->SetObjectField(TEXT("variable_name"), VariableParam);

		TSharedPtr<FJsonObject> CategoryParam = MakeShared<FJsonObject>();
		CategoryParam->SetStringField(TEXT("type"), TEXT("string"));
		CategoryParam->SetStringField(TEXT("description"), TEXT("Optional variable category."));
		Tool.Parameters->SetObjectField(TEXT("category"), CategoryParam);

		TSharedPtr<FJsonObject> TooltipParam = MakeShared<FJsonObject>();
		TooltipParam->SetStringField(TEXT("type"), TEXT("string"));
		TooltipParam->SetStringField(TEXT("description"), TEXT("Optional variable tooltip."));
		Tool.Parameters->SetObjectField(TEXT("tooltip"), TooltipParam);

		TSharedPtr<FJsonObject> AdvancedDisplayParam = MakeShared<FJsonObject>();
		AdvancedDisplayParam->SetStringField(TEXT("type"), TEXT("boolean"));
		AdvancedDisplayParam->SetStringField(TEXT("description"), TEXT("Optional advanced display flag."));
		Tool.Parameters->SetObjectField(TEXT("advanced_display"), AdvancedDisplayParam);

		TSharedPtr<FJsonObject> PrivateParam = MakeShared<FJsonObject>();
		PrivateParam->SetStringField(TEXT("type"), TEXT("boolean"));
		PrivateParam->SetStringField(TEXT("description"), TEXT("Optional private visibility flag."));
		Tool.Parameters->SetObjectField(TEXT("private"), PrivateParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("variable_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("set_variable_instance_editable");
		Tool.Description = TEXT("Set whether a variable is editable on instances.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> VariableParam = MakeShared<FJsonObject>();
		VariableParam->SetStringField(TEXT("type"), TEXT("string"));
		VariableParam->SetStringField(TEXT("description"), TEXT("Variable name."));
		Tool.Parameters->SetObjectField(TEXT("variable_name"), VariableParam);

		TSharedPtr<FJsonObject> EditableParam = MakeShared<FJsonObject>();
		EditableParam->SetStringField(TEXT("type"), TEXT("boolean"));
		EditableParam->SetStringField(TEXT("description"), TEXT("True to make the variable instance-editable."));
		Tool.Parameters->SetObjectField(TEXT("instance_editable"), EditableParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("variable_name"));
		Tool.RequiredParams.Add(TEXT("instance_editable"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("set_variable_expose_on_spawn");
		Tool.Description = TEXT("Set whether a variable is exposed on spawn nodes.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> VariableParam = MakeShared<FJsonObject>();
		VariableParam->SetStringField(TEXT("type"), TEXT("string"));
		VariableParam->SetStringField(TEXT("description"), TEXT("Variable name."));
		Tool.Parameters->SetObjectField(TEXT("variable_name"), VariableParam);

		TSharedPtr<FJsonObject> ExposeParam = MakeShared<FJsonObject>();
		ExposeParam->SetStringField(TEXT("type"), TEXT("boolean"));
		ExposeParam->SetStringField(TEXT("description"), TEXT("True to expose the variable on spawn."));
		Tool.Parameters->SetObjectField(TEXT("expose_on_spawn"), ExposeParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("variable_name"));
		Tool.RequiredParams.Add(TEXT("expose_on_spawn"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("set_variable_savegame");
		Tool.Description = TEXT("Set whether a variable is marked SaveGame.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> VariableParam = MakeShared<FJsonObject>();
		VariableParam->SetStringField(TEXT("type"), TEXT("string"));
		VariableParam->SetStringField(TEXT("description"), TEXT("Variable name."));
		Tool.Parameters->SetObjectField(TEXT("variable_name"), VariableParam);

		TSharedPtr<FJsonObject> SaveGameParam = MakeShared<FJsonObject>();
		SaveGameParam->SetStringField(TEXT("type"), TEXT("boolean"));
		SaveGameParam->SetStringField(TEXT("description"), TEXT("True to enable SaveGame flag."));
		Tool.Parameters->SetObjectField(TEXT("save_game"), SaveGameParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("variable_name"));
		Tool.RequiredParams.Add(TEXT("save_game"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("set_variable_transient");
		Tool.Description = TEXT("Set whether a variable is marked Transient.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> VariableParam = MakeShared<FJsonObject>();
		VariableParam->SetStringField(TEXT("type"), TEXT("string"));
		VariableParam->SetStringField(TEXT("description"), TEXT("Variable name."));
		Tool.Parameters->SetObjectField(TEXT("variable_name"), VariableParam);

		TSharedPtr<FJsonObject> TransientParam = MakeShared<FJsonObject>();
		TransientParam->SetStringField(TEXT("type"), TEXT("boolean"));
		TransientParam->SetStringField(TEXT("description"), TEXT("True to enable Transient flag."));
		Tool.Parameters->SetObjectField(TEXT("transient"), TransientParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("variable_name"));
		Tool.RequiredParams.Add(TEXT("transient"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("set_variable_replication");
		Tool.Description = TEXT("Set variable replication flags: replicated, rep_notify_function, replication_condition.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> VariableParam = MakeShared<FJsonObject>();
		VariableParam->SetStringField(TEXT("type"), TEXT("string"));
		VariableParam->SetStringField(TEXT("description"), TEXT("Variable name."));
		Tool.Parameters->SetObjectField(TEXT("variable_name"), VariableParam);

		TSharedPtr<FJsonObject> ReplicatedParam = MakeShared<FJsonObject>();
		ReplicatedParam->SetStringField(TEXT("type"), TEXT("boolean"));
		ReplicatedParam->SetStringField(TEXT("description"), TEXT("Optional replicated flag."));
		Tool.Parameters->SetObjectField(TEXT("replicated"), ReplicatedParam);

		TSharedPtr<FJsonObject> RepNotifyParam = MakeShared<FJsonObject>();
		RepNotifyParam->SetStringField(TEXT("type"), TEXT("string"));
		RepNotifyParam->SetStringField(TEXT("description"), TEXT("Optional RepNotify function name. Empty string clears RepNotify."));
		Tool.Parameters->SetObjectField(TEXT("rep_notify_function"), RepNotifyParam);

		TSharedPtr<FJsonObject> ConditionParam = MakeShared<FJsonObject>();
		ConditionParam->SetStringField(TEXT("type"), TEXT("string"));
		ConditionParam->SetStringField(TEXT("description"), TEXT("Optional replication condition (e.g. none, initial_only, owner_only, skip_owner)."));
		Tool.Parameters->SetObjectField(TEXT("replication_condition"), ConditionParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("variable_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("add_event_node");
		Tool.Description = TEXT("Add an event node to a Blueprint graph (e.g. BeginPlay, Tick).");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> EventParam = MakeShared<FJsonObject>();
		EventParam->SetStringField(TEXT("type"), TEXT("string"));
		EventParam->SetStringField(TEXT("description"), TEXT("Event name (BeginPlay, Tick, or native event function name)."));
		Tool.Parameters->SetObjectField(TEXT("event_name"), EventParam);

		TSharedPtr<FJsonObject> ClassParam = MakeShared<FJsonObject>();
		ClassParam->SetStringField(TEXT("type"), TEXT("string"));
		ClassParam->SetStringField(TEXT("description"), TEXT("Event owner class (default: /Script/Engine.Actor)."));
		Tool.Parameters->SetObjectField(TEXT("event_class"), ClassParam);

		TSharedPtr<FJsonObject> GraphParam = MakeShared<FJsonObject>();
		GraphParam->SetStringField(TEXT("type"), TEXT("string"));
		GraphParam->SetStringField(TEXT("description"), TEXT("Graph name (default: EventGraph)."));
		Tool.Parameters->SetObjectField(TEXT("graph_name"), GraphParam);

		TSharedPtr<FJsonObject> XParam = MakeShared<FJsonObject>();
		XParam->SetStringField(TEXT("type"), TEXT("number"));
		XParam->SetStringField(TEXT("description"), TEXT("Node X position (default: 0)."));
		Tool.Parameters->SetObjectField(TEXT("x"), XParam);

		TSharedPtr<FJsonObject> YParam = MakeShared<FJsonObject>();
		YParam->SetStringField(TEXT("type"), TEXT("number"));
		YParam->SetStringField(TEXT("description"), TEXT("Node Y position (default: 0)."));
		Tool.Parameters->SetObjectField(TEXT("y"), YParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("event_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("add_call_function_node");
		Tool.Description = TEXT("Add a Call Function node to a Blueprint graph.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> ClassParam = MakeShared<FJsonObject>();
		ClassParam->SetStringField(TEXT("type"), TEXT("string"));
		ClassParam->SetStringField(TEXT("description"), TEXT("Owning class for the function (e.g. /Script/Engine.Actor, Actor)."));
		Tool.Parameters->SetObjectField(TEXT("function_class"), ClassParam);

		TSharedPtr<FJsonObject> FunctionParam = MakeShared<FJsonObject>();
		FunctionParam->SetStringField(TEXT("type"), TEXT("string"));
		FunctionParam->SetStringField(TEXT("description"), TEXT("Function name."));
		Tool.Parameters->SetObjectField(TEXT("function_name"), FunctionParam);

		TSharedPtr<FJsonObject> GraphParam = MakeShared<FJsonObject>();
		GraphParam->SetStringField(TEXT("type"), TEXT("string"));
		GraphParam->SetStringField(TEXT("description"), TEXT("Graph name (default: EventGraph)."));
		Tool.Parameters->SetObjectField(TEXT("graph_name"), GraphParam);

		TSharedPtr<FJsonObject> XParam = MakeShared<FJsonObject>();
		XParam->SetStringField(TEXT("type"), TEXT("number"));
		XParam->SetStringField(TEXT("description"), TEXT("Node X position (default: 0)."));
		Tool.Parameters->SetObjectField(TEXT("x"), XParam);

		TSharedPtr<FJsonObject> YParam = MakeShared<FJsonObject>();
		YParam->SetStringField(TEXT("type"), TEXT("number"));
		YParam->SetStringField(TEXT("description"), TEXT("Node Y position (default: 0)."));
		Tool.Parameters->SetObjectField(TEXT("y"), YParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("function_class"));
		Tool.RequiredParams.Add(TEXT("function_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("add_variable_get_node");
		Tool.Description = TEXT("Add a variable getter node to a Blueprint graph.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> NameParam = MakeShared<FJsonObject>();
		NameParam->SetStringField(TEXT("type"), TEXT("string"));
		NameParam->SetStringField(TEXT("description"), TEXT("Blueprint variable name."));
		Tool.Parameters->SetObjectField(TEXT("variable_name"), NameParam);

		TSharedPtr<FJsonObject> GraphParam = MakeShared<FJsonObject>();
		GraphParam->SetStringField(TEXT("type"), TEXT("string"));
		GraphParam->SetStringField(TEXT("description"), TEXT("Graph name (default: EventGraph)."));
		Tool.Parameters->SetObjectField(TEXT("graph_name"), GraphParam);

		TSharedPtr<FJsonObject> XParam = MakeShared<FJsonObject>();
		XParam->SetStringField(TEXT("type"), TEXT("number"));
		XParam->SetStringField(TEXT("description"), TEXT("Node X position (default: 0)."));
		Tool.Parameters->SetObjectField(TEXT("x"), XParam);

		TSharedPtr<FJsonObject> YParam = MakeShared<FJsonObject>();
		YParam->SetStringField(TEXT("type"), TEXT("number"));
		YParam->SetStringField(TEXT("description"), TEXT("Node Y position (default: 0)."));
		Tool.Parameters->SetObjectField(TEXT("y"), YParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("variable_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("add_variable_set_node");
		Tool.Description = TEXT("Add a variable setter node to a Blueprint graph.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> NameParam = MakeShared<FJsonObject>();
		NameParam->SetStringField(TEXT("type"), TEXT("string"));
		NameParam->SetStringField(TEXT("description"), TEXT("Blueprint variable name."));
		Tool.Parameters->SetObjectField(TEXT("variable_name"), NameParam);

		TSharedPtr<FJsonObject> GraphParam = MakeShared<FJsonObject>();
		GraphParam->SetStringField(TEXT("type"), TEXT("string"));
		GraphParam->SetStringField(TEXT("description"), TEXT("Graph name (default: EventGraph)."));
		Tool.Parameters->SetObjectField(TEXT("graph_name"), GraphParam);

		TSharedPtr<FJsonObject> XParam = MakeShared<FJsonObject>();
		XParam->SetStringField(TEXT("type"), TEXT("number"));
		XParam->SetStringField(TEXT("description"), TEXT("Node X position (default: 0)."));
		Tool.Parameters->SetObjectField(TEXT("x"), XParam);

		TSharedPtr<FJsonObject> YParam = MakeShared<FJsonObject>();
		YParam->SetStringField(TEXT("type"), TEXT("number"));
		YParam->SetStringField(TEXT("description"), TEXT("Node Y position (default: 0)."));
		Tool.Parameters->SetObjectField(TEXT("y"), YParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("variable_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("set_pin_default_value");
		Tool.Description = TEXT("Set a node pin default value string.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> GraphParam = MakeShared<FJsonObject>();
		GraphParam->SetStringField(TEXT("type"), TEXT("string"));
		GraphParam->SetStringField(TEXT("description"), TEXT("Graph name (default: EventGraph)."));
		Tool.Parameters->SetObjectField(TEXT("graph_name"), GraphParam);

		TSharedPtr<FJsonObject> NodeParam = MakeShared<FJsonObject>();
		NodeParam->SetStringField(TEXT("type"), TEXT("string"));
		NodeParam->SetStringField(TEXT("description"), TEXT("Node id from list_graph_nodes/add_*_node."));
		Tool.Parameters->SetObjectField(TEXT("node_id"), NodeParam);

		TSharedPtr<FJsonObject> PinParam = MakeShared<FJsonObject>();
		PinParam->SetStringField(TEXT("type"), TEXT("string"));
		PinParam->SetStringField(TEXT("description"), TEXT("Pin name to edit."));
		Tool.Parameters->SetObjectField(TEXT("pin_name"), PinParam);

		TSharedPtr<FJsonObject> ValueParam = MakeShared<FJsonObject>();
		ValueParam->SetStringField(TEXT("type"), TEXT("string"));
		ValueParam->SetStringField(TEXT("description"), TEXT("Default value string (for rotator use format (Pitch=10,Yaw=10,Roll=10))."));
		Tool.Parameters->SetObjectField(TEXT("default_value"), ValueParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("node_id"));
		Tool.RequiredParams.Add(TEXT("pin_name"));
		Tool.RequiredParams.Add(TEXT("default_value"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("connect_pins");
		Tool.Description = TEXT("Connect two pins by node_id + pin names.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> GraphParam = MakeShared<FJsonObject>();
		GraphParam->SetStringField(TEXT("type"), TEXT("string"));
		GraphParam->SetStringField(TEXT("description"), TEXT("Graph name (default: EventGraph)."));
		Tool.Parameters->SetObjectField(TEXT("graph_name"), GraphParam);

		TSharedPtr<FJsonObject> FromNodeParam = MakeShared<FJsonObject>();
		FromNodeParam->SetStringField(TEXT("type"), TEXT("string"));
		FromNodeParam->SetStringField(TEXT("description"), TEXT("Source node_id."));
		Tool.Parameters->SetObjectField(TEXT("from_node_id"), FromNodeParam);

		TSharedPtr<FJsonObject> FromPinParam = MakeShared<FJsonObject>();
		FromPinParam->SetStringField(TEXT("type"), TEXT("string"));
		FromPinParam->SetStringField(TEXT("description"), TEXT("Source pin name."));
		Tool.Parameters->SetObjectField(TEXT("from_pin"), FromPinParam);

		TSharedPtr<FJsonObject> ToNodeParam = MakeShared<FJsonObject>();
		ToNodeParam->SetStringField(TEXT("type"), TEXT("string"));
		ToNodeParam->SetStringField(TEXT("description"), TEXT("Target node_id."));
		Tool.Parameters->SetObjectField(TEXT("to_node_id"), ToNodeParam);

		TSharedPtr<FJsonObject> ToPinParam = MakeShared<FJsonObject>();
		ToPinParam->SetStringField(TEXT("type"), TEXT("string"));
		ToPinParam->SetStringField(TEXT("description"), TEXT("Target pin name."));
		Tool.Parameters->SetObjectField(TEXT("to_pin"), ToPinParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("from_node_id"));
		Tool.RequiredParams.Add(TEXT("from_pin"));
		Tool.RequiredParams.Add(TEXT("to_node_id"));
		Tool.RequiredParams.Add(TEXT("to_pin"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("compile_blueprint");
		Tool.Description = TEXT("Compile a Blueprint after graph edits.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tools.Add(Tool);
	}

	return Tools;
}

FMCPResponse FBlueprintService::HandleRequest(const FMCPRequest& Request, const FString& MethodName)
{
	if (MethodName == TEXT("create_blueprint")) return HandleCreateBlueprint(Request);
	if (MethodName == TEXT("duplicate_blueprint")) return HandleDuplicateBlueprint(Request);
	if (MethodName == TEXT("rename_blueprint")) return HandleRenameBlueprint(Request);
	if (MethodName == TEXT("delete_blueprint")) return HandleDeleteBlueprint(Request);
	if (MethodName == TEXT("save_blueprint")) return HandleSaveBlueprint(Request);
	if (MethodName == TEXT("reparent_blueprint")) return HandleReparentBlueprint(Request);
	if (MethodName == TEXT("get_blueprint_info")) return HandleGetBlueprintInfo(Request);
	if (MethodName == TEXT("set_class_settings")) return HandleSetClassSettings(Request);
	if (MethodName == TEXT("list_graphs")) return HandleListGraphs(Request);
	if (MethodName == TEXT("create_graph")) return HandleCreateGraph(Request);
	if (MethodName == TEXT("rename_graph")) return HandleRenameGraph(Request);
	if (MethodName == TEXT("delete_graph")) return HandleDeleteGraph(Request);
	if (MethodName == TEXT("set_graph_metadata")) return HandleSetGraphMetadata(Request);
	if (MethodName == TEXT("format_graph")) return HandleFormatGraph(Request);
	if (MethodName == TEXT("list_graph_nodes")) return HandleListGraphNodes(Request);
	if (MethodName == TEXT("create_variable")) return HandleCreateVariable(Request);
	if (MethodName == TEXT("list_variables")) return HandleListVariables(Request);
	if (MethodName == TEXT("rename_variable")) return HandleRenameVariable(Request);
	if (MethodName == TEXT("delete_variable")) return HandleDeleteVariable(Request);
	if (MethodName == TEXT("set_variable_default")) return HandleSetVariableDefault(Request);
	if (MethodName == TEXT("set_variable_metadata")) return HandleSetVariableMetadata(Request);
	if (MethodName == TEXT("set_variable_instance_editable")) return HandleSetVariableInstanceEditable(Request);
	if (MethodName == TEXT("set_variable_expose_on_spawn")) return HandleSetVariableExposeOnSpawn(Request);
	if (MethodName == TEXT("set_variable_savegame")) return HandleSetVariableSaveGame(Request);
	if (MethodName == TEXT("set_variable_transient")) return HandleSetVariableTransient(Request);
	if (MethodName == TEXT("set_variable_replication")) return HandleSetVariableReplication(Request);
	if (MethodName == TEXT("add_event_node")) return HandleAddEventNode(Request);
	if (MethodName == TEXT("add_call_function_node")) return HandleAddCallFunctionNode(Request);
	if (MethodName == TEXT("add_variable_get_node")) return HandleAddVariableGetNode(Request);
	if (MethodName == TEXT("add_variable_set_node")) return HandleAddVariableSetNode(Request);
	if (MethodName == TEXT("set_pin_default_value")) return HandleSetPinDefaultValue(Request);
	if (MethodName == TEXT("connect_pins")) return HandleConnectPins(Request);
	if (MethodName == TEXT("compile_blueprint")) return HandleCompileBlueprint(Request);

	return MethodNotFound(Request.Id, TEXT("blueprint"), MethodName);
}

FString FBlueprintService::NormalizeBlueprintPath(const FString& BlueprintPath)
{
	const FString AssetPath = NormalizeBlueprintAssetPath(BlueprintPath);
	if (AssetPath.IsEmpty())
	{
		return AssetPath;
	}

	const FString AssetName = FPackageName::GetShortName(AssetPath);
	return FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
}

FString FBlueprintService::NormalizeBlueprintAssetPath(const FString& BlueprintPath)
{
	FString Normalized = BlueprintPath.TrimStartAndEnd();
	if (Normalized.IsEmpty())
	{
		return Normalized;
	}

	if (Normalized.Contains(TEXT(".")))
	{
		FString PackagePath;
		FString ObjectName;
		if (Normalized.Split(TEXT("."), &PackagePath, &ObjectName, ESearchCase::CaseSensitive, ESearchDir::FromStart))
		{
			Normalized = PackagePath;
		}
	}

	if (Normalized.EndsWith(TEXT("_C"), ESearchCase::CaseSensitive))
	{
		Normalized.LeftChopInline(2);
	}

	return Normalized;
}

UBlueprint* FBlueprintService::LoadBlueprint(const FString& BlueprintPath)
{
	const FString NormalizedPath = NormalizeBlueprintPath(BlueprintPath);
	return Cast<UBlueprint>(StaticLoadObject(UBlueprint::StaticClass(), nullptr, *NormalizedPath));
}

UClass* FBlueprintService::ResolveClass(const FString& ClassNameOrPath)
{
	if (ClassNameOrPath.IsEmpty())
	{
		return nullptr;
	}

	if (UClass* PathClass = FindObject<UClass>(nullptr, *ClassNameOrPath))
	{
		return PathClass;
	}
	if (UClass* LoadedPathClass = LoadObject<UClass>(nullptr, *ClassNameOrPath))
	{
		return LoadedPathClass;
	}

	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Candidate = *It;
		if (!Candidate)
		{
			continue;
		}
		if (Candidate->GetName().Equals(ClassNameOrPath, ESearchCase::CaseSensitive))
		{
			return Candidate;
		}
	}

	return nullptr;
}

UEdGraph* FBlueprintService::ResolveGraph(UBlueprint* Blueprint, const FString& GraphName)
{
	if (!Blueprint)
	{
		return nullptr;
	}

	if (GraphName.IsEmpty() || GraphName.Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase))
	{
		return FBlueprintEditorUtils::FindEventGraph(Blueprint);
	}

	TArray<UEdGraph*> AllGraphs;
	Blueprint->GetAllGraphs(AllGraphs);
	for (UEdGraph* Graph : AllGraphs)
	{
		if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
		{
			return Graph;
		}
	}

	return nullptr;
}

UEdGraphNode* FBlueprintService::FindNodeById(UEdGraph* Graph, const FString& NodeId)
{
	if (!Graph)
	{
		return nullptr;
	}

	FGuid NodeGuid;
	if (!FGuid::Parse(NodeId, NodeGuid))
	{
		return nullptr;
	}

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->NodeGuid == NodeGuid)
		{
			return Node;
		}
	}
	return nullptr;
}

UEdGraphPin* FBlueprintService::FindPinByName(UEdGraphNode* Node, const FString& PinName)
{
	if (!Node)
	{
		return nullptr;
	}
	return Node->FindPin(FName(*PinName), EGPD_MAX);
}

FMCPResponse FBlueprintService::HandleCreateBlueprint(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString ParentClassPath = TEXT("/Script/Engine.Actor");
	FString BlueprintTypeName = TEXT("normal");

	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	Request.Params->TryGetStringField(TEXT("parent_class"), ParentClassPath);
	Request.Params->TryGetStringField(TEXT("blueprint_type"), BlueprintTypeName);

	EBlueprintType BlueprintType = BPTYPE_Normal;
	FString BlueprintTypeError;
	if (!ParseBlueprintType(BlueprintTypeName, BlueprintType, BlueprintTypeError))
	{
		return InvalidParams(Request.Id, BlueprintTypeError);
	}

	auto Task = [BlueprintPath, ParentClassPath, BlueprintType]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		auto Fail = [&Result](const FString& Error) -> TSharedPtr<FJsonObject>
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		};

		const FString AssetPath = NormalizeBlueprintAssetPath(BlueprintPath);
		if (AssetPath.IsEmpty())
		{
			return Fail(TEXT("Invalid 'blueprint_path'"));
		}
		if (!FPackageName::IsValidLongPackageName(AssetPath))
		{
			return Fail(FString::Printf(TEXT("Invalid blueprint package path: %s"), *AssetPath));
		}
		if (UEditorAssetLibrary::DoesAssetExist(AssetPath))
		{
			return Fail(FString::Printf(TEXT("Blueprint already exists: %s"), *AssetPath));
		}

		UClass* ParentClass = ResolveClass(ParentClassPath);
		if (!ParentClass)
		{
			return Fail(FString::Printf(TEXT("Parent class not found: %s"), *ParentClassPath));
		}
		if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
		{
			return Fail(FString::Printf(TEXT("Cannot create Blueprint from parent class: %s"), *ParentClass->GetPathName()));
		}

		UPackage* Package = CreatePackage(*AssetPath);
		if (!Package)
		{
			return Fail(FString::Printf(TEXT("Failed to create package: %s"), *AssetPath));
		}

		const FName AssetName(*FPackageName::GetShortName(AssetPath));
		UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
			ParentClass,
			Package,
			AssetName,
			BlueprintType,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			FName(TEXT("SpecialAgent"))
		);

		if (!Blueprint)
		{
			return Fail(FString::Printf(TEXT("Failed to create Blueprint: %s"), *AssetPath));
		}

		FAssetRegistryModule::AssetCreated(Blueprint);
		Blueprint->MarkPackageDirty();

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(AssetPath));
		Result->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());
		Result->SetStringField(TEXT("blueprint_type"), BlueprintTypeToString(Blueprint->BlueprintType));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleDuplicateBlueprint(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString SourceBlueprintPath;
	FString DestinationBlueprintPath;
	if (!Request.Params->TryGetStringField(TEXT("source_blueprint_path"), SourceBlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'source_blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("destination_blueprint_path"), DestinationBlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'destination_blueprint_path'"));
	}

	auto Task = [SourceBlueprintPath, DestinationBlueprintPath]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		auto Fail = [&Result](const FString& Error) -> TSharedPtr<FJsonObject>
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		};

		const FString SourceAssetPath = NormalizeBlueprintAssetPath(SourceBlueprintPath);
		const FString DestinationAssetPath = NormalizeBlueprintAssetPath(DestinationBlueprintPath);

		if (!FPackageName::IsValidLongPackageName(SourceAssetPath))
		{
			return Fail(FString::Printf(TEXT("Invalid source path: %s"), *SourceBlueprintPath));
		}
		if (!FPackageName::IsValidLongPackageName(DestinationAssetPath))
		{
			return Fail(FString::Printf(TEXT("Invalid destination path: %s"), *DestinationBlueprintPath));
		}
		if (!UEditorAssetLibrary::DoesAssetExist(SourceAssetPath))
		{
			return Fail(FString::Printf(TEXT("Source Blueprint not found: %s"), *SourceAssetPath));
		}
		if (UEditorAssetLibrary::DoesAssetExist(DestinationAssetPath))
		{
			return Fail(FString::Printf(TEXT("Destination Blueprint already exists: %s"), *DestinationAssetPath));
		}

		UObject* DuplicatedAsset = UEditorAssetLibrary::DuplicateAsset(SourceAssetPath, DestinationAssetPath);
		UBlueprint* DuplicatedBlueprint = Cast<UBlueprint>(DuplicatedAsset);
		if (!DuplicatedBlueprint)
		{
			DuplicatedBlueprint = LoadBlueprint(DestinationAssetPath);
		}
		if (!DuplicatedBlueprint)
		{
			return Fail(FString::Printf(TEXT("Failed to duplicate Blueprint from %s to %s"), *SourceAssetPath, *DestinationAssetPath));
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("source_blueprint_path"), NormalizeBlueprintPath(SourceAssetPath));
		Result->SetStringField(TEXT("destination_blueprint_path"), NormalizeBlueprintPath(DestinationAssetPath));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleRenameBlueprint(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString NewBlueprintPath;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("new_blueprint_path"), NewBlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'new_blueprint_path'"));
	}

	auto Task = [BlueprintPath, NewBlueprintPath]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		auto Fail = [&Result](const FString& Error) -> TSharedPtr<FJsonObject>
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		};

		const FString SourceAssetPath = NormalizeBlueprintAssetPath(BlueprintPath);
		const FString DestinationAssetPath = NormalizeBlueprintAssetPath(NewBlueprintPath);

		if (!FPackageName::IsValidLongPackageName(SourceAssetPath))
		{
			return Fail(FString::Printf(TEXT("Invalid source path: %s"), *BlueprintPath));
		}
		if (!FPackageName::IsValidLongPackageName(DestinationAssetPath))
		{
			return Fail(FString::Printf(TEXT("Invalid destination path: %s"), *NewBlueprintPath));
		}
		if (!UEditorAssetLibrary::DoesAssetExist(SourceAssetPath))
		{
			return Fail(FString::Printf(TEXT("Blueprint not found: %s"), *SourceAssetPath));
		}
		if (UEditorAssetLibrary::DoesAssetExist(DestinationAssetPath))
		{
			return Fail(FString::Printf(TEXT("Destination already exists: %s"), *DestinationAssetPath));
		}

		if (!UEditorAssetLibrary::RenameAsset(SourceAssetPath, DestinationAssetPath))
		{
			return Fail(FString::Printf(TEXT("Failed to rename Blueprint from %s to %s"), *SourceAssetPath, *DestinationAssetPath));
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("old_blueprint_path"), NormalizeBlueprintPath(SourceAssetPath));
		Result->SetStringField(TEXT("new_blueprint_path"), NormalizeBlueprintPath(DestinationAssetPath));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleDeleteBlueprint(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}

	auto Task = [BlueprintPath]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		auto Fail = [&Result](const FString& Error) -> TSharedPtr<FJsonObject>
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		};

		const FString AssetPath = NormalizeBlueprintAssetPath(BlueprintPath);
		if (!FPackageName::IsValidLongPackageName(AssetPath))
		{
			return Fail(FString::Printf(TEXT("Invalid blueprint path: %s"), *BlueprintPath));
		}
		if (!UEditorAssetLibrary::DoesAssetExist(AssetPath))
		{
			return Fail(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
		}
		if (!UEditorAssetLibrary::DeleteAsset(AssetPath))
		{
			return Fail(FString::Printf(TEXT("Failed to delete Blueprint: %s"), *AssetPath));
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(AssetPath));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleSaveBlueprint(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	bool bOnlyIfDirty = true;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	Request.Params->TryGetBoolField(TEXT("only_if_dirty"), bOnlyIfDirty);

	auto Task = [BlueprintPath, bOnlyIfDirty]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		auto Fail = [&Result](const FString& Error) -> TSharedPtr<FJsonObject>
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		};

		const FString AssetPath = NormalizeBlueprintAssetPath(BlueprintPath);
		if (!FPackageName::IsValidLongPackageName(AssetPath))
		{
			return Fail(FString::Printf(TEXT("Invalid blueprint path: %s"), *BlueprintPath));
		}
		if (!UEditorAssetLibrary::DoesAssetExist(AssetPath))
		{
			return Fail(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
		}

		const bool bSaved = UEditorAssetLibrary::SaveAsset(AssetPath, bOnlyIfDirty);
		if (!bSaved)
		{
			return Fail(FString::Printf(TEXT("Failed to save Blueprint: %s"), *AssetPath));
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(AssetPath));
		Result->SetBoolField(TEXT("only_if_dirty"), bOnlyIfDirty);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleReparentBlueprint(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString ParentClassPath;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("parent_class"), ParentClassPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'parent_class'"));
	}

	auto Task = [BlueprintPath, ParentClassPath]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		auto Fail = [&Result](const FString& Error) -> TSharedPtr<FJsonObject>
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		};

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			return Fail(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		}

		UClass* NewParentClass = ResolveClass(ParentClassPath);
		if (!NewParentClass)
		{
			return Fail(FString::Printf(TEXT("Parent class not found: %s"), *ParentClassPath));
		}
		if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(NewParentClass))
		{
			return Fail(FString::Printf(TEXT("Cannot reparent Blueprint to class: %s"), *NewParentClass->GetPathName()));
		}

		const FString OldParentClassPath = Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT("None");
		if (Blueprint->ParentClass == NewParentClass)
		{
			Result->SetBoolField(TEXT("success"), true);
			Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
			Result->SetStringField(TEXT("old_parent_class"), OldParentClassPath);
			Result->SetStringField(TEXT("new_parent_class"), NewParentClass->GetPathName());
			Result->SetBoolField(TEXT("changed"), false);
			return Result;
		}

		UBlueprintEditorLibrary::ReparentBlueprint(Blueprint, NewParentClass);
		if (Blueprint->ParentClass != NewParentClass)
		{
			return Fail(FString::Printf(TEXT("Reparent failed. Blueprint parent remained: %s"), *OldParentClassPath));
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetStringField(TEXT("old_parent_class"), OldParentClassPath);
		Result->SetStringField(TEXT("new_parent_class"), NewParentClass->GetPathName());
		Result->SetBoolField(TEXT("changed"), true);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleGetBlueprintInfo(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}

	auto Task = [BlueprintPath]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		auto Fail = [&Result](const FString& Error) -> TSharedPtr<FJsonObject>
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		};

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			return Fail(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		}

		UClass* GeneratedClass = Blueprint->GeneratedClass;
		const UClass* SkeletonClass = Blueprint->SkeletonGeneratedClass;

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetStringField(TEXT("asset_path"), NormalizeBlueprintAssetPath(BlueprintPath));
		Result->SetStringField(TEXT("asset_name"), Blueprint->GetName());
		Result->SetStringField(TEXT("package_name"), Blueprint->GetOutermost() ? Blueprint->GetOutermost()->GetName() : TEXT("None"));
		Result->SetStringField(TEXT("blueprint_type"), BlueprintTypeToString(Blueprint->BlueprintType));
		Result->SetNumberField(TEXT("status"), static_cast<int32>(Blueprint->Status));
		Result->SetStringField(TEXT("status_name"), BlueprintStatusToString(Blueprint->Status));
		Result->SetBoolField(TEXT("is_data_only"), FBlueprintEditorUtils::IsDataOnlyBlueprint(Blueprint));
		Result->SetBoolField(TEXT("generate_const_class"), Blueprint->bGenerateConstClass);
		Result->SetBoolField(TEXT("generate_abstract_class"), Blueprint->bGenerateAbstractClass);
		Result->SetBoolField(TEXT("deprecated"), Blueprint->bDeprecate);
		Result->SetBoolField(TEXT("run_construction_script_on_drag"), Blueprint->bRunConstructionScriptOnDrag);

		Result->SetNumberField(TEXT("variable_count"), Blueprint->NewVariables.Num());
		Result->SetNumberField(TEXT("event_graph_count"), Blueprint->EventGraphs.Num());
		Result->SetNumberField(TEXT("function_graph_count"), Blueprint->FunctionGraphs.Num());
		Result->SetNumberField(TEXT("macro_graph_count"), Blueprint->MacroGraphs.Num());

		if (Blueprint->ParentClass)
		{
			Result->SetStringField(TEXT("parent_class"), Blueprint->ParentClass->GetPathName());
		}
		if (GeneratedClass)
		{
			Result->SetStringField(TEXT("generated_class"), GeneratedClass->GetPathName());
		}
		if (SkeletonClass)
		{
			Result->SetStringField(TEXT("skeleton_class"), SkeletonClass->GetPathName());
		}

		TSharedPtr<FJsonObject> ClassFlagsObj = MakeShared<FJsonObject>();
		ClassFlagsObj->SetBoolField(TEXT("config"), GeneratedClass ? GeneratedClass->HasAnyClassFlags(CLASS_Config) : false);
		ClassFlagsObj->SetBoolField(TEXT("default_config"), GeneratedClass ? GeneratedClass->HasAnyClassFlags(CLASS_DefaultConfig) : false);
		ClassFlagsObj->SetBoolField(TEXT("config_do_not_check_defaults"), GeneratedClass ? GeneratedClass->HasAnyClassFlags(CLASS_ConfigDoNotCheckDefaults) : false);
		ClassFlagsObj->SetBoolField(TEXT("not_placeable"), GeneratedClass ? GeneratedClass->HasAnyClassFlags(CLASS_NotPlaceable) : false);
		ClassFlagsObj->SetBoolField(TEXT("abstract"), Blueprint->bGenerateAbstractClass);
		ClassFlagsObj->SetBoolField(TEXT("const_class"), Blueprint->bGenerateConstClass);
		ClassFlagsObj->SetBoolField(TEXT("deprecated"), Blueprint->bDeprecate);
		Result->SetObjectField(TEXT("class_flags"), ClassFlagsObj);

		if (GeneratedClass)
		{
			if (const AActor* ActorCDO = Cast<AActor>(GeneratedClass->GetDefaultObject()))
			{
				TSharedPtr<FJsonObject> TickObj = MakeShared<FJsonObject>();
				TickObj->SetBoolField(TEXT("enabled"), ActorCDO->PrimaryActorTick.bCanEverTick);
				TickObj->SetBoolField(TEXT("start_enabled"), ActorCDO->PrimaryActorTick.bStartWithTickEnabled);
				TickObj->SetNumberField(TEXT("interval"), ActorCDO->PrimaryActorTick.TickInterval);
				Result->SetObjectField(TEXT("tick"), TickObj);

				TSharedPtr<FJsonObject> ReplicationObj = MakeShared<FJsonObject>();
				ReplicationObj->SetBoolField(TEXT("replicates"), ActorCDO->GetIsReplicated());
				ReplicationObj->SetBoolField(TEXT("replicate_movement"), ActorCDO->IsReplicatingMovement());
				ReplicationObj->SetBoolField(TEXT("net_load_on_client"), ActorCDO->bNetLoadOnClient);
				ReplicationObj->SetBoolField(TEXT("always_relevant"), ActorCDO->bAlwaysRelevant);
				ReplicationObj->SetBoolField(TEXT("only_relevant_to_owner"), ActorCDO->bOnlyRelevantToOwner);
				ReplicationObj->SetBoolField(TEXT("use_owner_relevancy"), ActorCDO->bNetUseOwnerRelevancy);
				ReplicationObj->SetNumberField(TEXT("net_update_frequency"), ActorCDO->GetNetUpdateFrequency());
				ReplicationObj->SetNumberField(TEXT("min_net_update_frequency"), ActorCDO->GetMinNetUpdateFrequency());
				ReplicationObj->SetNumberField(TEXT("net_cull_distance_squared"), ActorCDO->GetNetCullDistanceSquared());
				Result->SetObjectField(TEXT("replication"), ReplicationObj);

				TSharedPtr<FJsonObject> InputObj = MakeShared<FJsonObject>();
				InputObj->SetStringField(TEXT("auto_receive_input"), AutoReceiveInputToString(ActorCDO->AutoReceiveInput));
				InputObj->SetNumberField(TEXT("auto_receive_input_index"), static_cast<int32>(ActorCDO->AutoReceiveInput.GetValue()));
				InputObj->SetNumberField(TEXT("input_priority"), ActorCDO->InputPriority);
				InputObj->SetBoolField(TEXT("block_input"), ActorCDO->bBlockInput);
				Result->SetObjectField(TEXT("input"), InputObj);
			}
		}

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleSetClassSettings(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}

	TSharedPtr<FJsonObject> TickSettings;
	TSharedPtr<FJsonObject> ReplicationSettings;
	TSharedPtr<FJsonObject> InputSettings;
	TSharedPtr<FJsonObject> ClassFlagSettings;

	const TSharedPtr<FJsonObject>* TickSettingsPtr = nullptr;
	if (Request.Params->TryGetObjectField(TEXT("tick"), TickSettingsPtr))
	{
		TickSettings = *TickSettingsPtr;
	}

	const TSharedPtr<FJsonObject>* ReplicationSettingsPtr = nullptr;
	if (Request.Params->TryGetObjectField(TEXT("replication"), ReplicationSettingsPtr))
	{
		ReplicationSettings = *ReplicationSettingsPtr;
	}

	const TSharedPtr<FJsonObject>* InputSettingsPtr = nullptr;
	if (Request.Params->TryGetObjectField(TEXT("input"), InputSettingsPtr))
	{
		InputSettings = *InputSettingsPtr;
	}

	const TSharedPtr<FJsonObject>* ClassFlagSettingsPtr = nullptr;
	if (Request.Params->TryGetObjectField(TEXT("class_flags"), ClassFlagSettingsPtr))
	{
		ClassFlagSettings = *ClassFlagSettingsPtr;
	}

	if (!TickSettings.IsValid() && !ReplicationSettings.IsValid() && !InputSettings.IsValid() && !ClassFlagSettings.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Provide at least one of: tick, replication, input, class_flags"));
	}

	auto Task = [BlueprintPath, TickSettings, ReplicationSettings, InputSettings, ClassFlagSettings]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		auto Fail = [&Result](const FString& Error) -> TSharedPtr<FJsonObject>
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		};

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			return Fail(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		}

		if (!Blueprint->GeneratedClass)
		{
			FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, nullptr);
		}

		UClass* GeneratedClass = Blueprint->GeneratedClass;
		UClass* SkeletonClass = Blueprint->SkeletonGeneratedClass;
		AActor* ActorCDO = GeneratedClass ? Cast<AActor>(GeneratedClass->GetDefaultObject()) : nullptr;

		const bool bRequestedTick = TickSettings.IsValid() && TickSettings->Values.Num() > 0;
		const bool bRequestedReplication = ReplicationSettings.IsValid() && ReplicationSettings->Values.Num() > 0;
		const bool bRequestedInput = InputSettings.IsValid() && InputSettings->Values.Num() > 0;
		const bool bRequestedActorSettings = bRequestedTick || bRequestedReplication || bRequestedInput;

		if (bRequestedActorSettings && !ActorCDO)
		{
			return Fail(TEXT("tick/replication/input settings require an Actor Blueprint"));
		}

		bool bModified = false;
		bool bStructuralModified = false;
		bool bActorModified = false;
		bool bBlueprintModified = false;
		bool bGeneratedClassModified = false;
		bool bSkeletonClassModified = false;

		auto EnsureActorModified = [&]()
		{
			if (!bActorModified && ActorCDO)
			{
				ActorCDO->Modify();
				bActorModified = true;
			}
		};

		auto EnsureBlueprintModified = [&]()
		{
			if (!bBlueprintModified)
			{
				Blueprint->Modify();
				bBlueprintModified = true;
			}
		};

		auto EnsureGeneratedClassModified = [&]()
		{
			if (!bGeneratedClassModified && GeneratedClass)
			{
				GeneratedClass->Modify();
				bGeneratedClassModified = true;
			}
		};

		auto EnsureSkeletonClassModified = [&]()
		{
			if (!bSkeletonClassModified && SkeletonClass)
			{
				SkeletonClass->Modify();
				bSkeletonClassModified = true;
			}
		};

		if (bRequestedTick)
		{
			if (TickSettings->HasField(TEXT("enabled")))
			{
				bool bEnabled = false;
				if (!TickSettings->TryGetBoolField(TEXT("enabled"), bEnabled))
				{
					return Fail(TEXT("tick.enabled must be a boolean"));
				}
				EnsureActorModified();
				ActorCDO->PrimaryActorTick.bCanEverTick = bEnabled;
				bModified = true;
			}

			if (TickSettings->HasField(TEXT("start_enabled")))
			{
				bool bStartEnabled = false;
				if (!TickSettings->TryGetBoolField(TEXT("start_enabled"), bStartEnabled))
				{
					return Fail(TEXT("tick.start_enabled must be a boolean"));
				}
				EnsureActorModified();
				ActorCDO->PrimaryActorTick.bStartWithTickEnabled = bStartEnabled;
				bModified = true;
			}

			if (TickSettings->HasField(TEXT("interval")))
			{
				double TickInterval = 0.0;
				if (!TickSettings->TryGetNumberField(TEXT("interval"), TickInterval))
				{
					return Fail(TEXT("tick.interval must be a number"));
				}
				EnsureActorModified();
				ActorCDO->PrimaryActorTick.TickInterval = FMath::Max(0.0, TickInterval);
				bModified = true;
			}
		}

		if (bRequestedReplication)
		{
			if (ReplicationSettings->HasField(TEXT("replicates")))
			{
				bool bReplicates = false;
				if (!ReplicationSettings->TryGetBoolField(TEXT("replicates"), bReplicates))
				{
					return Fail(TEXT("replication.replicates must be a boolean"));
				}
				EnsureActorModified();
				ActorCDO->SetReplicates(bReplicates);
				bModified = true;
			}

			if (ReplicationSettings->HasField(TEXT("replicate_movement")))
			{
				bool bReplicateMovement = false;
				if (!ReplicationSettings->TryGetBoolField(TEXT("replicate_movement"), bReplicateMovement))
				{
					return Fail(TEXT("replication.replicate_movement must be a boolean"));
				}
				EnsureActorModified();
				ActorCDO->SetReplicateMovement(bReplicateMovement);
				bModified = true;
			}

			if (ReplicationSettings->HasField(TEXT("net_load_on_client")))
			{
				bool bNetLoadOnClient = false;
				if (!ReplicationSettings->TryGetBoolField(TEXT("net_load_on_client"), bNetLoadOnClient))
				{
					return Fail(TEXT("replication.net_load_on_client must be a boolean"));
				}
				EnsureActorModified();
				ActorCDO->bNetLoadOnClient = bNetLoadOnClient;
				bModified = true;
			}

			if (ReplicationSettings->HasField(TEXT("always_relevant")))
			{
				bool bAlwaysRelevant = false;
				if (!ReplicationSettings->TryGetBoolField(TEXT("always_relevant"), bAlwaysRelevant))
				{
					return Fail(TEXT("replication.always_relevant must be a boolean"));
				}
				EnsureActorModified();
				ActorCDO->bAlwaysRelevant = bAlwaysRelevant;
				bModified = true;
			}

			if (ReplicationSettings->HasField(TEXT("only_relevant_to_owner")))
			{
				bool bOnlyRelevantToOwner = false;
				if (!ReplicationSettings->TryGetBoolField(TEXT("only_relevant_to_owner"), bOnlyRelevantToOwner))
				{
					return Fail(TEXT("replication.only_relevant_to_owner must be a boolean"));
				}
				EnsureActorModified();
				ActorCDO->bOnlyRelevantToOwner = bOnlyRelevantToOwner;
				bModified = true;
			}

			if (ReplicationSettings->HasField(TEXT("use_owner_relevancy")))
			{
				bool bUseOwnerRelevancy = false;
				if (!ReplicationSettings->TryGetBoolField(TEXT("use_owner_relevancy"), bUseOwnerRelevancy))
				{
					return Fail(TEXT("replication.use_owner_relevancy must be a boolean"));
				}
				EnsureActorModified();
				ActorCDO->bNetUseOwnerRelevancy = bUseOwnerRelevancy;
				bModified = true;
			}

			if (ReplicationSettings->HasField(TEXT("net_update_frequency")))
			{
				double NetUpdateFrequency = 0.0;
				if (!ReplicationSettings->TryGetNumberField(TEXT("net_update_frequency"), NetUpdateFrequency))
				{
					return Fail(TEXT("replication.net_update_frequency must be a number"));
				}
				EnsureActorModified();
				ActorCDO->SetNetUpdateFrequency(FMath::Max(0.0, NetUpdateFrequency));
				bModified = true;
			}

			if (ReplicationSettings->HasField(TEXT("min_net_update_frequency")))
			{
				double MinNetUpdateFrequency = 0.0;
				if (!ReplicationSettings->TryGetNumberField(TEXT("min_net_update_frequency"), MinNetUpdateFrequency))
				{
					return Fail(TEXT("replication.min_net_update_frequency must be a number"));
				}
				EnsureActorModified();
				ActorCDO->SetMinNetUpdateFrequency(FMath::Max(0.0, MinNetUpdateFrequency));
				bModified = true;
			}

			if (ReplicationSettings->HasField(TEXT("net_cull_distance_squared")))
			{
				double NetCullDistanceSquared = 0.0;
				if (!ReplicationSettings->TryGetNumberField(TEXT("net_cull_distance_squared"), NetCullDistanceSquared))
				{
					return Fail(TEXT("replication.net_cull_distance_squared must be a number"));
				}
				EnsureActorModified();
				ActorCDO->SetNetCullDistanceSquared(FMath::Max(0.0, NetCullDistanceSquared));
				bModified = true;
			}
		}

		if (bRequestedInput)
		{
			if (InputSettings->HasField(TEXT("auto_receive_input")))
			{
				EAutoReceiveInput::Type AutoReceiveInput = EAutoReceiveInput::Disabled;
				bool bParsedInputMode = false;

				FString AutoReceiveInputString;
				if (InputSettings->TryGetStringField(TEXT("auto_receive_input"), AutoReceiveInputString))
				{
					bParsedInputMode = ParseAutoReceiveInput(AutoReceiveInputString, AutoReceiveInput);
				}
				else
				{
					double AutoReceiveInputIndex = 0.0;
					if (InputSettings->TryGetNumberField(TEXT("auto_receive_input"), AutoReceiveInputIndex))
					{
						const int32 ClampedInputIndex = FMath::Clamp(FMath::RoundToInt(AutoReceiveInputIndex), 0, 8);
						AutoReceiveInput = static_cast<EAutoReceiveInput::Type>(ClampedInputIndex);
						bParsedInputMode = true;
					}
				}

				if (!bParsedInputMode)
				{
					return Fail(TEXT("input.auto_receive_input must be disabled/player0..player7 or number 0..8"));
				}

				EnsureActorModified();
				ActorCDO->AutoReceiveInput = AutoReceiveInput;
				bModified = true;
			}

			if (InputSettings->HasField(TEXT("input_priority")))
			{
				double InputPriority = 0.0;
				if (!InputSettings->TryGetNumberField(TEXT("input_priority"), InputPriority))
				{
					return Fail(TEXT("input.input_priority must be a number"));
				}
				EnsureActorModified();
				ActorCDO->InputPriority = FMath::RoundToInt(InputPriority);
				bModified = true;
			}

			if (InputSettings->HasField(TEXT("block_input")))
			{
				bool bBlockInput = false;
				if (!InputSettings->TryGetBoolField(TEXT("block_input"), bBlockInput))
				{
					return Fail(TEXT("input.block_input must be a boolean"));
				}
				EnsureActorModified();
				ActorCDO->bBlockInput = bBlockInput;
				bModified = true;
			}
		}

		if (ClassFlagSettings.IsValid() && ClassFlagSettings->Values.Num() > 0)
		{
			if (ClassFlagSettings->HasField(TEXT("abstract")))
			{
				bool bAbstract = false;
				if (!ClassFlagSettings->TryGetBoolField(TEXT("abstract"), bAbstract))
				{
					return Fail(TEXT("class_flags.abstract must be a boolean"));
				}
				EnsureBlueprintModified();
				Blueprint->bGenerateAbstractClass = bAbstract;
				bModified = true;
				bStructuralModified = true;
			}

			if (ClassFlagSettings->HasField(TEXT("const_class")))
			{
				bool bConstClass = false;
				if (!ClassFlagSettings->TryGetBoolField(TEXT("const_class"), bConstClass))
				{
					return Fail(TEXT("class_flags.const_class must be a boolean"));
				}
				EnsureBlueprintModified();
				Blueprint->bGenerateConstClass = bConstClass;
				bModified = true;
				bStructuralModified = true;
			}

			if (ClassFlagSettings->HasField(TEXT("deprecated")))
			{
				bool bDeprecated = false;
				if (!ClassFlagSettings->TryGetBoolField(TEXT("deprecated"), bDeprecated))
				{
					return Fail(TEXT("class_flags.deprecated must be a boolean"));
				}
				EnsureBlueprintModified();
				Blueprint->bDeprecate = bDeprecated;
				bModified = true;
				bStructuralModified = true;
			}

			if (ClassFlagSettings->HasField(TEXT("run_construction_script_on_drag")))
			{
				bool bRunConstructionScriptOnDrag = false;
				if (!ClassFlagSettings->TryGetBoolField(TEXT("run_construction_script_on_drag"), bRunConstructionScriptOnDrag))
				{
					return Fail(TEXT("class_flags.run_construction_script_on_drag must be a boolean"));
				}
				EnsureBlueprintModified();
				Blueprint->bRunConstructionScriptOnDrag = bRunConstructionScriptOnDrag;
				bModified = true;
			}

			const bool bWantsConfig = ClassFlagSettings->HasField(TEXT("config"));
			const bool bWantsDefaultConfig = ClassFlagSettings->HasField(TEXT("default_config"));
			const bool bWantsConfigDoNotCheckDefaults = ClassFlagSettings->HasField(TEXT("config_do_not_check_defaults"));
			const bool bWantsNotPlaceable = ClassFlagSettings->HasField(TEXT("not_placeable"));
			const bool bWantsGeneratedClassFlags = bWantsConfig || bWantsDefaultConfig || bWantsConfigDoNotCheckDefaults || bWantsNotPlaceable;

			if (bWantsGeneratedClassFlags && (!GeneratedClass || !SkeletonClass))
			{
				FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, nullptr);
				GeneratedClass = Blueprint->GeneratedClass;
				SkeletonClass = Blueprint->SkeletonGeneratedClass;
			}

			if (bWantsGeneratedClassFlags && (!GeneratedClass || !SkeletonClass))
			{
				return Fail(TEXT("Unable to resolve generated/skeleton class to apply class_flags"));
			}

			auto ApplyGeneratedClassFlag = [&](const TCHAR* FieldName, const EClassFlags ClassFlag) -> bool
			{
				if (!ClassFlagSettings->HasField(FieldName))
				{
					return true;
				}

				bool bEnabled = false;
				if (!ClassFlagSettings->TryGetBoolField(FieldName, bEnabled))
				{
					Result->SetBoolField(TEXT("success"), false);
					Result->SetStringField(TEXT("error"), FString::Printf(TEXT("class_flags.%s must be a boolean"), FieldName));
					return false;
				}

				EnsureGeneratedClassModified();
				EnsureSkeletonClassModified();
				SetClassFlag(GeneratedClass, ClassFlag, bEnabled);
				SetClassFlag(SkeletonClass, ClassFlag, bEnabled);
				bModified = true;
				bStructuralModified = true;
				return true;
			};

			if (!ApplyGeneratedClassFlag(TEXT("config"), CLASS_Config))
			{
				return Result;
			}
			if (!ApplyGeneratedClassFlag(TEXT("default_config"), CLASS_DefaultConfig))
			{
				return Result;
			}
			if (!ApplyGeneratedClassFlag(TEXT("config_do_not_check_defaults"), CLASS_ConfigDoNotCheckDefaults))
			{
				return Result;
			}
			if (!ApplyGeneratedClassFlag(TEXT("not_placeable"), CLASS_NotPlaceable))
			{
				return Result;
			}
		}

		if (!bModified && !bStructuralModified)
		{
			return Fail(TEXT("No valid class settings were applied"));
		}

		if (bStructuralModified)
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		}
		else
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		}

		GeneratedClass = Blueprint->GeneratedClass;
		ActorCDO = GeneratedClass ? Cast<AActor>(GeneratedClass->GetDefaultObject()) : nullptr;

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetBoolField(TEXT("structural_change"), bStructuralModified);

		TSharedPtr<FJsonObject> AppliedClassFlags = MakeShared<FJsonObject>();
		AppliedClassFlags->SetBoolField(TEXT("abstract"), Blueprint->bGenerateAbstractClass);
		AppliedClassFlags->SetBoolField(TEXT("const_class"), Blueprint->bGenerateConstClass);
		AppliedClassFlags->SetBoolField(TEXT("deprecated"), Blueprint->bDeprecate);
		AppliedClassFlags->SetBoolField(TEXT("run_construction_script_on_drag"), Blueprint->bRunConstructionScriptOnDrag);
		AppliedClassFlags->SetBoolField(TEXT("config"), GeneratedClass ? GeneratedClass->HasAnyClassFlags(CLASS_Config) : false);
		AppliedClassFlags->SetBoolField(TEXT("default_config"), GeneratedClass ? GeneratedClass->HasAnyClassFlags(CLASS_DefaultConfig) : false);
		AppliedClassFlags->SetBoolField(TEXT("config_do_not_check_defaults"), GeneratedClass ? GeneratedClass->HasAnyClassFlags(CLASS_ConfigDoNotCheckDefaults) : false);
		AppliedClassFlags->SetBoolField(TEXT("not_placeable"), GeneratedClass ? GeneratedClass->HasAnyClassFlags(CLASS_NotPlaceable) : false);
		Result->SetObjectField(TEXT("class_flags"), AppliedClassFlags);

		if (ActorCDO)
		{
			TSharedPtr<FJsonObject> TickObj = MakeShared<FJsonObject>();
			TickObj->SetBoolField(TEXT("enabled"), ActorCDO->PrimaryActorTick.bCanEverTick);
			TickObj->SetBoolField(TEXT("start_enabled"), ActorCDO->PrimaryActorTick.bStartWithTickEnabled);
			TickObj->SetNumberField(TEXT("interval"), ActorCDO->PrimaryActorTick.TickInterval);
			Result->SetObjectField(TEXT("tick"), TickObj);

			TSharedPtr<FJsonObject> ReplicationObj = MakeShared<FJsonObject>();
			ReplicationObj->SetBoolField(TEXT("replicates"), ActorCDO->GetIsReplicated());
			ReplicationObj->SetBoolField(TEXT("replicate_movement"), ActorCDO->IsReplicatingMovement());
			ReplicationObj->SetBoolField(TEXT("net_load_on_client"), ActorCDO->bNetLoadOnClient);
			ReplicationObj->SetBoolField(TEXT("always_relevant"), ActorCDO->bAlwaysRelevant);
			ReplicationObj->SetBoolField(TEXT("only_relevant_to_owner"), ActorCDO->bOnlyRelevantToOwner);
			ReplicationObj->SetBoolField(TEXT("use_owner_relevancy"), ActorCDO->bNetUseOwnerRelevancy);
			ReplicationObj->SetNumberField(TEXT("net_update_frequency"), ActorCDO->GetNetUpdateFrequency());
			ReplicationObj->SetNumberField(TEXT("min_net_update_frequency"), ActorCDO->GetMinNetUpdateFrequency());
			ReplicationObj->SetNumberField(TEXT("net_cull_distance_squared"), ActorCDO->GetNetCullDistanceSquared());
			Result->SetObjectField(TEXT("replication"), ReplicationObj);

			TSharedPtr<FJsonObject> InputObj = MakeShared<FJsonObject>();
			InputObj->SetStringField(TEXT("auto_receive_input"), AutoReceiveInputToString(ActorCDO->AutoReceiveInput));
			InputObj->SetNumberField(TEXT("auto_receive_input_index"), static_cast<int32>(ActorCDO->AutoReceiveInput.GetValue()));
			InputObj->SetNumberField(TEXT("input_priority"), ActorCDO->InputPriority);
			InputObj->SetBoolField(TEXT("block_input"), ActorCDO->bBlockInput);
			Result->SetObjectField(TEXT("input"), InputObj);
		}

		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleListGraphs(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}

	auto Task = [BlueprintPath]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return Result;
		}

		TArray<UEdGraph*> Graphs;
		Graphs.Reserve(
			Blueprint->UbergraphPages.Num() +
			Blueprint->FunctionGraphs.Num() +
			Blueprint->MacroGraphs.Num() +
			Blueprint->DelegateSignatureGraphs.Num()
		);

		TSet<UEdGraph*> SeenGraphs;
		auto AddGraphUnique = [&Graphs, &SeenGraphs](UEdGraph* Graph)
		{
			if (Graph && !SeenGraphs.Contains(Graph))
			{
				SeenGraphs.Add(Graph);
				Graphs.Add(Graph);
			}
		};

		for (UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			AddGraphUnique(Graph);
		}
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			AddGraphUnique(Graph);
		}
		for (UEdGraph* Graph : Blueprint->MacroGraphs)
		{
			AddGraphUnique(Graph);
		}
		for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
		{
			AddGraphUnique(Graph);
		}
		for (const FBPInterfaceDescription& InterfaceDescription : Blueprint->ImplementedInterfaces)
		{
			for (UEdGraph* Graph : InterfaceDescription.Graphs)
			{
				AddGraphUnique(Graph);
			}
		}

		Graphs.Sort([](const UEdGraph& A, const UEdGraph& B)
		{
			return A.GetName().Compare(B.GetName(), ESearchCase::IgnoreCase) < 0;
		});

		TArray<TSharedPtr<FJsonValue>> GraphsJson;
		for (const UEdGraph* Graph : Graphs)
		{
			GraphsJson.Add(MakeShared<FJsonValueObject>(BuildGraphJson(Blueprint, Graph)));
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetArrayField(TEXT("graphs"), GraphsJson);
		Result->SetNumberField(TEXT("count"), GraphsJson.Num());
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleCreateGraph(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString GraphName;
	FString GraphTypeName = TEXT("function");

	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("graph_name"), GraphName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'graph_name'"));
	}
	Request.Params->TryGetStringField(TEXT("graph_type"), GraphTypeName);

	ECreateGraphType GraphType = ECreateGraphType::Function;
	FString GraphTypeError;
	if (!ParseGraphType(GraphTypeName, GraphType, GraphTypeError))
	{
		return InvalidParams(Request.Id, GraphTypeError);
	}

	auto Task = [BlueprintPath, GraphName, GraphType]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		auto Fail = [&Result](const FString& Error) -> TSharedPtr<FJsonObject>
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		};

		const FString TrimmedGraphName = GraphName.TrimStartAndEnd();
		if (TrimmedGraphName.IsEmpty())
		{
			return Fail(TEXT("Graph name cannot be empty"));
		}

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			return Fail(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		}

		if (ResolveGraph(Blueprint, TrimmedGraphName))
		{
			return Fail(FString::Printf(TEXT("Graph already exists: %s"), *TrimmedGraphName));
		}

		UEdGraph* CreatedGraph = nullptr;
		switch (GraphType)
		{
			case ECreateGraphType::Function:
			{
				CreatedGraph = UBlueprintEditorLibrary::AddFunctionGraph(Blueprint, TrimmedGraphName);
				break;
			}
			case ECreateGraphType::Macro:
			{
				CreatedGraph = FBlueprintEditorUtils::CreateNewGraph(
					Blueprint,
					FName(*TrimmedGraphName),
					UEdGraph::StaticClass(),
					UEdGraphSchema_K2::StaticClass()
				);
				if (CreatedGraph)
				{
					FBlueprintEditorUtils::AddMacroGraph(Blueprint, CreatedGraph, true, nullptr);
				}
				break;
			}
			case ECreateGraphType::Event:
			{
				CreatedGraph = FBlueprintEditorUtils::CreateNewGraph(
					Blueprint,
					FName(*TrimmedGraphName),
					UEdGraph::StaticClass(),
					UEdGraphSchema_K2::StaticClass()
				);
				if (CreatedGraph)
				{
					FBlueprintEditorUtils::AddUbergraphPage(Blueprint, CreatedGraph);
					if (const UEdGraphSchema* Schema = CreatedGraph->GetSchema())
					{
						Schema->CreateDefaultNodesForGraph(*CreatedGraph);
					}
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				}
				break;
			}
		}

		if (!CreatedGraph)
		{
			return Fail(FString::Printf(TEXT("Failed to create graph '%s'"), *TrimmedGraphName));
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetObjectField(TEXT("graph"), BuildGraphJson(Blueprint, CreatedGraph));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleRenameGraph(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString GraphName;
	FString NewGraphName;

	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("graph_name"), GraphName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'graph_name'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("new_graph_name"), NewGraphName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'new_graph_name'"));
	}

	auto Task = [BlueprintPath, GraphName, NewGraphName]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		auto Fail = [&Result](const FString& Error) -> TSharedPtr<FJsonObject>
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		};

		const FString TrimmedGraphName = GraphName.TrimStartAndEnd();
		const FString TrimmedNewGraphName = NewGraphName.TrimStartAndEnd();
		if (TrimmedGraphName.IsEmpty() || TrimmedNewGraphName.IsEmpty())
		{
			return Fail(TEXT("graph_name and new_graph_name must be non-empty"));
		}

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			return Fail(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		}

		UEdGraph* Graph = ResolveGraph(Blueprint, TrimmedGraphName);
		if (!Graph)
		{
			return Fail(FString::Printf(TEXT("Graph not found: %s"), *TrimmedGraphName));
		}

		if (Graph->GetName().Equals(TrimmedNewGraphName, ESearchCase::IgnoreCase))
		{
			Result->SetBoolField(TEXT("success"), true);
			Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
			Result->SetObjectField(TEXT("graph"), BuildGraphJson(Blueprint, Graph));
			Result->SetBoolField(TEXT("changed"), false);
			return Result;
		}

		if (UEdGraph* ExistingGraph = ResolveGraph(Blueprint, TrimmedNewGraphName))
		{
			if (ExistingGraph != Graph)
			{
				return Fail(FString::Printf(TEXT("A graph already exists with name: %s"), *TrimmedNewGraphName));
			}
		}

		const FString OldGraphName = Graph->GetName();
		FBlueprintEditorUtils::RenameGraph(Graph, TrimmedNewGraphName);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetStringField(TEXT("old_graph_name"), OldGraphName);
		Result->SetObjectField(TEXT("graph"), BuildGraphJson(Blueprint, Graph));
		Result->SetBoolField(TEXT("changed"), true);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleDeleteGraph(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString GraphName;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("graph_name"), GraphName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'graph_name'"));
	}

	auto Task = [BlueprintPath, GraphName]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		auto Fail = [&Result](const FString& Error) -> TSharedPtr<FJsonObject>
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		};

		const FString TrimmedGraphName = GraphName.TrimStartAndEnd();
		if (TrimmedGraphName.IsEmpty())
		{
			return Fail(TEXT("graph_name must be non-empty"));
		}

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			return Fail(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		}

		UEdGraph* Graph = ResolveGraph(Blueprint, TrimmedGraphName);
		if (!Graph)
		{
			return Fail(FString::Printf(TEXT("Graph not found: %s"), *TrimmedGraphName));
		}

		const FString RemovedGraphName = Graph->GetName();
		const FString RemovedGraphType = GraphTypeToString(GetBlueprintGraphType(Blueprint, Graph));
		FBlueprintEditorUtils::RemoveGraph(Blueprint, Graph, EGraphRemoveFlags::Default);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetStringField(TEXT("removed_graph_name"), RemovedGraphName);
		Result->SetStringField(TEXT("removed_graph_type"), RemovedGraphType);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleSetGraphMetadata(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString GraphName;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("graph_name"), GraphName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'graph_name'"));
	}

	FString Category;
	FString Tooltip;
	FString Access;
	const bool bHasCategory = Request.Params->TryGetStringField(TEXT("category"), Category);
	const bool bHasTooltip = Request.Params->TryGetStringField(TEXT("tooltip"), Tooltip);
	const bool bHasAccess = Request.Params->TryGetStringField(TEXT("access"), Access);

	if (!bHasCategory && !bHasTooltip && !bHasAccess)
	{
		return InvalidParams(Request.Id, TEXT("Provide at least one of: category, tooltip, access"));
	}

	uint32 ParsedAccessSpecifier = FUNC_Public;
	if (bHasAccess)
	{
		FString AccessError;
		if (!ParseAccessSpecifier(Access, ParsedAccessSpecifier, AccessError))
		{
			return InvalidParams(Request.Id, AccessError);
		}
	}

	auto Task = [BlueprintPath, GraphName, bHasCategory, Category, bHasTooltip, Tooltip, bHasAccess, ParsedAccessSpecifier]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		auto Fail = [&Result](const FString& Error) -> TSharedPtr<FJsonObject>
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		};

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			return Fail(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		}

		UEdGraph* Graph = ResolveGraph(Blueprint, GraphName);
		if (!Graph)
		{
			return Fail(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
		}

		FKismetUserDeclaredFunctionMetadata* Metadata = FBlueprintEditorUtils::GetGraphFunctionMetaData(Graph);
		if (!Metadata)
		{
			return Fail(TEXT("Graph metadata is not available for this graph type"));
		}

		bool bChanged = false;
		bool bStructuralChanged = false;

		if (bHasCategory)
		{
			const FText NewCategory = Category.TrimStartAndEnd().IsEmpty() ? UEdGraphSchema_K2::VR_DefaultCategory : FText::FromString(Category.TrimStartAndEnd());
			if (!Metadata->Category.EqualTo(NewCategory))
			{
				FBlueprintEditorUtils::SetBlueprintFunctionOrMacroCategory(Graph, NewCategory, true);
				bChanged = true;
			}
		}

		if (bHasTooltip)
		{
			const FString TrimmedTooltip = Tooltip.TrimStartAndEnd();
			const FText NewTooltip = FText::FromString(TrimmedTooltip);
			if (!Metadata->ToolTip.EqualTo(NewTooltip))
			{
				FBlueprintEditorUtils::ModifyFunctionMetaData(Graph);
				Metadata->ToolTip = NewTooltip;
				if (Blueprint->SkeletonGeneratedClass)
				{
					if (UFunction* Function = Blueprint->SkeletonGeneratedClass->FindFunctionByName(Graph->GetFName()))
					{
						Function->Modify();
						Function->SetMetaData(FBlueprintMetadata::MD_Tooltip, *TrimmedTooltip);
					}
				}
				bChanged = true;
			}
		}

		if (bHasAccess)
		{
			UK2Node_FunctionEntry* FunctionEntry = Cast<UK2Node_FunctionEntry>(FBlueprintEditorUtils::GetEntryNode(Graph));
			if (!FunctionEntry)
			{
				return Fail(TEXT("access can only be set on function graphs"));
			}

			const int32 ExistingExtraFlags = FunctionEntry->GetExtraFlags();
			const int32 UpdatedExtraFlags = (ExistingExtraFlags & ~FUNC_AccessSpecifiers) | ParsedAccessSpecifier;
			if (UpdatedExtraFlags != ExistingExtraFlags)
			{
				FunctionEntry->Modify();
				FunctionEntry->SetExtraFlags(UpdatedExtraFlags);
				if (Blueprint->SkeletonGeneratedClass)
				{
					if (UFunction* Function = Blueprint->SkeletonGeneratedClass->FindFunctionByName(Graph->GetFName()))
					{
						Function->Modify();
						const uint32 ExistingFlags = static_cast<uint32>(Function->FunctionFlags);
						const uint32 UpdatedFlags = (ExistingFlags & ~FUNC_AccessSpecifiers) | ParsedAccessSpecifier;
						Function->FunctionFlags = static_cast<EFunctionFlags>(UpdatedFlags);
					}
				}
				bChanged = true;
				bStructuralChanged = true;
			}
		}

		if (!bChanged)
		{
			return Fail(TEXT("No graph metadata changes were applied"));
		}

		if (bStructuralChanged)
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		}
		else
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetObjectField(TEXT("graph"), BuildGraphJson(Blueprint, Graph));
		Result->SetBoolField(TEXT("structural_change"), bStructuralChanged);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleFormatGraph(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString GraphName;
	int32 StartX = 0;
	int32 StartY = 0;
	int32 XSpacing = 420;
	int32 YSpacing = 220;

	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("graph_name"), GraphName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'graph_name'"));
	}
	Request.Params->TryGetNumberField(TEXT("start_x"), StartX);
	Request.Params->TryGetNumberField(TEXT("start_y"), StartY);
	Request.Params->TryGetNumberField(TEXT("x_spacing"), XSpacing);
	Request.Params->TryGetNumberField(TEXT("y_spacing"), YSpacing);

	auto Task = [BlueprintPath, GraphName, StartX, StartY, XSpacing, YSpacing]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		auto Fail = [&Result](const FString& Error) -> TSharedPtr<FJsonObject>
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		};

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			return Fail(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		}

		UEdGraph* Graph = ResolveGraph(Blueprint, GraphName);
		if (!Graph)
		{
			return Fail(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
		}

		TArray<UEdGraphNode*> Nodes;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node)
			{
				Nodes.Add(Node);
			}
		}

		if (Nodes.Num() == 0)
		{
			Result->SetBoolField(TEXT("success"), true);
			Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
			Result->SetObjectField(TEXT("graph"), BuildGraphJson(Blueprint, Graph));
			Result->SetNumberField(TEXT("formatted_nodes"), 0);
			return Result;
		}

		const int32 ClampedXSpacing = FMath::Max(120, XSpacing);
		const int32 ClampedYSpacing = FMath::Max(80, YSpacing);

		TSet<UEdGraphNode*> NodeSet;
		for (UEdGraphNode* Node : Nodes)
		{
			NodeSet.Add(Node);
		}

		TMap<UEdGraphNode*, TSet<UEdGraphNode*>> ExecAdjacency;
		TMap<UEdGraphNode*, int32> InDegree;
		for (UEdGraphNode* Node : Nodes)
		{
			InDegree.Add(Node, 0);
		}

		for (UEdGraphNode* Node : Nodes)
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				{
					continue;
				}

				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (!LinkedPin)
					{
						continue;
					}

					UEdGraphNode* TargetNode = LinkedPin->GetOwningNode();
					if (!TargetNode || TargetNode == Node || !NodeSet.Contains(TargetNode))
					{
						continue;
					}

					TSet<UEdGraphNode*>& Targets = ExecAdjacency.FindOrAdd(Node);
					if (!Targets.Contains(TargetNode))
					{
						Targets.Add(TargetNode);
						InDegree.FindOrAdd(TargetNode)++;
					}
				}
			}
		}

		TMap<UEdGraphNode*, int32> Depths;
		TArray<UEdGraphNode*> Queue;
		Queue.Reserve(Nodes.Num());

		for (UEdGraphNode* Node : Nodes)
		{
			if (InDegree.FindRef(Node) == 0)
			{
				Queue.Add(Node);
				Depths.Add(Node, 0);
			}
		}

		if (Queue.Num() == 0)
		{
			Nodes.Sort([](const UEdGraphNode& A, const UEdGraphNode& B)
			{
				if (A.NodePosY != B.NodePosY)
				{
					return A.NodePosY < B.NodePosY;
				}
				return A.NodePosX < B.NodePosX;
			});

			for (UEdGraphNode* Node : Nodes)
			{
				Depths.Add(Node, 0);
			}
		}
		else
		{
			int32 QueueIndex = 0;
			while (QueueIndex < Queue.Num())
			{
				UEdGraphNode* Node = Queue[QueueIndex++];
				const int32 NodeDepth = Depths.FindRef(Node);
				TSet<UEdGraphNode*>* Targets = ExecAdjacency.Find(Node);
				if (!Targets)
				{
					continue;
				}

				for (UEdGraphNode* TargetNode : *Targets)
				{
					int32& TargetDepth = Depths.FindOrAdd(TargetNode, 0);
					TargetDepth = FMath::Max(TargetDepth, NodeDepth + 1);

					int32& TargetInDegree = InDegree.FindOrAdd(TargetNode);
					TargetInDegree = FMath::Max(0, TargetInDegree - 1);
					if (TargetInDegree == 0)
					{
						Queue.Add(TargetNode);
					}
				}
			}

			for (UEdGraphNode* Node : Nodes)
			{
				Depths.FindOrAdd(Node, 0);
			}
		}

		TMap<int32, TArray<UEdGraphNode*>> NodesByDepth;
		for (UEdGraphNode* Node : Nodes)
		{
			const int32 Depth = Depths.FindRef(Node);
			NodesByDepth.FindOrAdd(Depth).Add(Node);
		}

		TArray<int32> OrderedDepths;
		NodesByDepth.GetKeys(OrderedDepths);
		OrderedDepths.Sort();

		int32 FormattedNodes = 0;
		for (int32 Depth : OrderedDepths)
		{
			TArray<UEdGraphNode*>& DepthNodes = NodesByDepth.FindChecked(Depth);
			DepthNodes.Sort([](const UEdGraphNode& A, const UEdGraphNode& B)
			{
				if (A.NodePosY != B.NodePosY)
				{
					return A.NodePosY < B.NodePosY;
				}
				return A.NodePosX < B.NodePosX;
			});

			for (int32 Index = 0; Index < DepthNodes.Num(); ++Index)
			{
				UEdGraphNode* Node = DepthNodes[Index];
				const int32 NewX = StartX + (Depth * ClampedXSpacing);
				const int32 NewY = StartY + (Index * ClampedYSpacing);

				if (Node->NodePosX != NewX || Node->NodePosY != NewY)
				{
					Node->Modify();
					Node->NodePosX = NewX;
					Node->NodePosY = NewY;
					++FormattedNodes;
				}
			}
		}

		if (FormattedNodes > 0)
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetObjectField(TEXT("graph"), BuildGraphJson(Blueprint, Graph));
		Result->SetNumberField(TEXT("formatted_nodes"), FormattedNodes);
		Result->SetNumberField(TEXT("x_spacing"), ClampedXSpacing);
		Result->SetNumberField(TEXT("y_spacing"), ClampedYSpacing);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleListGraphNodes(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}

	FString GraphName = TEXT("EventGraph");
	Request.Params->TryGetStringField(TEXT("graph_name"), GraphName);

	auto Task = [BlueprintPath, GraphName]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return Result;
		}

		UEdGraph* Graph = ResolveGraph(Blueprint, GraphName);
		if (!Graph)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Graph not found: %s"), *GraphName));
			return Result;
		}

		TArray<TSharedPtr<FJsonValue>> NodesJson;
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}
			NodesJson.Add(MakeShared<FJsonValueObject>(BuildNodeJson(Node)));
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetStringField(TEXT("graph_name"), Graph->GetName());
		Result->SetArrayField(TEXT("nodes"), NodesJson);
		Result->SetNumberField(TEXT("count"), NodesJson.Num());
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleCreateVariable(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString VariableName;
	FString VariableType;
	FString TypeReferencePath;
	FString ContainerTypeName;
	FString MapKeyTypeName;
	FString MapKeyTypeReferencePath;
	FString DefaultValue;

	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("variable_name"), VariableName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'variable_name'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("variable_type"), VariableType))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'variable_type'"));
	}
	Request.Params->TryGetStringField(TEXT("type_reference"), TypeReferencePath);
	const bool bHasContainerTypeOverride = Request.Params->TryGetStringField(TEXT("container_type"), ContainerTypeName);
	const bool bHasMapKeyType = Request.Params->TryGetStringField(TEXT("map_key_type"), MapKeyTypeName);
	Request.Params->TryGetStringField(TEXT("map_key_type_reference"), MapKeyTypeReferencePath);
	Request.Params->TryGetStringField(TEXT("default_value"), DefaultValue);

	EPinContainerType ContainerTypeOverride = EPinContainerType::None;
	if (bHasContainerTypeOverride)
	{
		FString ContainerTypeError;
		if (!ParsePinContainerType(ContainerTypeName, ContainerTypeOverride, ContainerTypeError))
		{
			return InvalidParams(Request.Id, ContainerTypeError);
		}
	}

	auto Task = [BlueprintPath, VariableName, VariableType, TypeReferencePath, bHasContainerTypeOverride, ContainerTypeOverride, bHasMapKeyType, MapKeyTypeName, MapKeyTypeReferencePath, DefaultValue]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return Result;
		}

		FString ParsedValueTypeName;
		FString ParsedMapKeyTypeName;
		FString ParseContainerError;
		EPinContainerType ParsedContainerType = EPinContainerType::None;
		if (!ParseContainerFromVariableType(VariableType, ParsedValueTypeName, ParsedContainerType, ParsedMapKeyTypeName, ParseContainerError))
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), ParseContainerError);
			return Result;
		}

		const EPinContainerType FinalContainerType = bHasContainerTypeOverride ? ContainerTypeOverride : ParsedContainerType;
		const FString ValueTypeName = ParsedValueTypeName.TrimStartAndEnd();
		if (ValueTypeName.IsEmpty())
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Resolved variable value type is empty"));
			return Result;
		}

		FEdGraphPinType ValuePinType;
		FString TypeError;
		if (!BuildPinType(ValueTypeName, TypeReferencePath, ValuePinType, TypeError))
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TypeError);
			return Result;
		}
		if (ValuePinType.ContainerType != EPinContainerType::None)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Nested container variable types are not supported"));
			return Result;
		}

		FEdGraphPinType NewVariablePinType = ValuePinType;
		NewVariablePinType.ContainerType = FinalContainerType;
		NewVariablePinType.PinValueType = FEdGraphTerminalType();

		if (FinalContainerType == EPinContainerType::Map)
		{
			const FString EffectiveMapKeyTypeName = bHasMapKeyType ? MapKeyTypeName.TrimStartAndEnd() : ParsedMapKeyTypeName.TrimStartAndEnd();
			if (EffectiveMapKeyTypeName.IsEmpty())
			{
				Result->SetBoolField(TEXT("success"), false);
				Result->SetStringField(TEXT("error"), TEXT("Map variables require map_key_type (or map<key_type,value_type> syntax)"));
				return Result;
			}

			FEdGraphPinType KeyPinType;
			FString KeyTypeError;
			if (!BuildPinType(EffectiveMapKeyTypeName, MapKeyTypeReferencePath, KeyPinType, KeyTypeError))
			{
				Result->SetBoolField(TEXT("success"), false);
				Result->SetStringField(TEXT("error"), KeyTypeError);
				return Result;
			}
			if (KeyPinType.ContainerType != EPinContainerType::None)
			{
				Result->SetBoolField(TEXT("success"), false);
				Result->SetStringField(TEXT("error"), TEXT("Map key type cannot be a container"));
				return Result;
			}

			NewVariablePinType.PinCategory = KeyPinType.PinCategory;
			NewVariablePinType.PinSubCategory = KeyPinType.PinSubCategory;
			NewVariablePinType.PinSubCategoryObject = KeyPinType.PinSubCategoryObject;
			NewVariablePinType.PinSubCategoryMemberReference = KeyPinType.PinSubCategoryMemberReference;
			NewVariablePinType.bIsReference = KeyPinType.bIsReference;
			NewVariablePinType.bIsConst = KeyPinType.bIsConst;
			NewVariablePinType.bIsWeakPointer = KeyPinType.bIsWeakPointer;
			NewVariablePinType.bIsUObjectWrapper = KeyPinType.bIsUObjectWrapper;
			NewVariablePinType.bSerializeAsSinglePrecisionFloat = KeyPinType.bSerializeAsSinglePrecisionFloat;

			NewVariablePinType.PinValueType.TerminalCategory = ValuePinType.PinCategory;
			NewVariablePinType.PinValueType.TerminalSubCategory = ValuePinType.PinSubCategory;
			NewVariablePinType.PinValueType.TerminalSubCategoryObject = ValuePinType.PinSubCategoryObject;
			NewVariablePinType.PinValueType.bTerminalIsConst = ValuePinType.bIsConst;
			NewVariablePinType.PinValueType.bTerminalIsWeakPointer = ValuePinType.bIsWeakPointer;
			NewVariablePinType.PinValueType.bTerminalIsUObjectWrapper = ValuePinType.bIsUObjectWrapper;
		}

		const bool bAdded = FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*VariableName), NewVariablePinType, DefaultValue);
		if (!bAdded)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to add variable '%s'"), *VariableName));
			return Result;
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		const int32 NewVariableIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(*VariableName));
		if (NewVariableIndex == INDEX_NONE)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Variable '%s' was added but could not be resolved"), *VariableName));
			return Result;
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetObjectField(TEXT("variable"), BuildVariableJson(Blueprint, Blueprint->NewVariables[NewVariableIndex]));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleListVariables(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}

	auto Task = [BlueprintPath]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return Result;
		}

		TArray<TSharedPtr<FJsonValue>> VariablesJson;
		for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
		{
			VariablesJson.Add(MakeShared<FJsonValueObject>(BuildVariableJson(Blueprint, Variable)));
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetArrayField(TEXT("variables"), VariablesJson);
		Result->SetNumberField(TEXT("count"), VariablesJson.Num());
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleRenameVariable(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString VariableName;
	FString NewVariableName;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("variable_name"), VariableName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'variable_name'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("new_variable_name"), NewVariableName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'new_variable_name'"));
	}

	auto Task = [BlueprintPath, VariableName, NewVariableName]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return Result;
		}

		const FName OldVarName(*VariableName);
		const FName NewVarName(*NewVariableName);
		const int32 OldIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, OldVarName);
		if (OldIndex == INDEX_NONE)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Variable not found: %s"), *VariableName));
			return Result;
		}
		if (OldVarName != NewVarName && FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, NewVarName) != INDEX_NONE)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Variable already exists: %s"), *NewVariableName));
			return Result;
		}

		FBlueprintEditorUtils::RenameMemberVariable(Blueprint, OldVarName, NewVarName);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		const int32 NewIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, NewVarName);
		if (NewIndex == INDEX_NONE)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to rename variable '%s' to '%s'"), *VariableName, *NewVariableName));
			return Result;
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetStringField(TEXT("old_variable_name"), VariableName);
		Result->SetStringField(TEXT("new_variable_name"), NewVariableName);
		Result->SetObjectField(TEXT("variable"), BuildVariableJson(Blueprint, Blueprint->NewVariables[NewIndex]));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleDeleteVariable(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString VariableName;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("variable_name"), VariableName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'variable_name'"));
	}

	auto Task = [BlueprintPath, VariableName]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return Result;
		}

		const FName VarName(*VariableName);
		if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName) == INDEX_NONE)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Variable not found: %s"), *VariableName));
			return Result;
		}

		FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, VarName);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		Result->SetBoolField(TEXT("success"), FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName) == INDEX_NONE);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetStringField(TEXT("variable_name"), VariableName);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleSetVariableDefault(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString VariableName;
	FString DefaultValue;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("variable_name"), VariableName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'variable_name'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("default_value"), DefaultValue))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'default_value'"));
	}

	auto Task = [BlueprintPath, VariableName, DefaultValue]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return Result;
		}

		const FName VarName(*VariableName);
		const int32 VariableIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName);
		if (VariableIndex == INDEX_NONE)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Variable not found: %s"), *VariableName));
			return Result;
		}

		Blueprint->Modify();
		Blueprint->NewVariables[VariableIndex].DefaultValue = DefaultValue;
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetObjectField(TEXT("variable"), BuildVariableJson(Blueprint, Blueprint->NewVariables[VariableIndex]));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleSetVariableMetadata(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString VariableName;
	FString Category;
	FString Tooltip;
	bool bAdvancedDisplay = false;
	bool bPrivate = false;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("variable_name"), VariableName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'variable_name'"));
	}

	const bool bHasCategory = Request.Params->TryGetStringField(TEXT("category"), Category);
	const bool bHasTooltip = Request.Params->TryGetStringField(TEXT("tooltip"), Tooltip);
	const bool bHasAdvancedDisplay = Request.Params->TryGetBoolField(TEXT("advanced_display"), bAdvancedDisplay);
	const bool bHasPrivate = Request.Params->TryGetBoolField(TEXT("private"), bPrivate);
	if (!bHasCategory && !bHasTooltip && !bHasAdvancedDisplay && !bHasPrivate)
	{
		return InvalidParams(Request.Id, TEXT("At least one of 'category', 'tooltip', 'advanced_display', or 'private' is required"));
	}

	auto Task = [BlueprintPath, VariableName, bHasCategory, Category, bHasTooltip, Tooltip, bHasAdvancedDisplay, bAdvancedDisplay, bHasPrivate, bPrivate]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return Result;
		}

		const FName VarName(*VariableName);
		const int32 VariableIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName);
		if (VariableIndex == INDEX_NONE)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Variable not found: %s"), *VariableName));
			return Result;
		}

		Blueprint->Modify();
		if (bHasCategory)
		{
			FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, VarName, nullptr, FText::FromString(Category));
		}
		if (bHasTooltip)
		{
			if (Tooltip.IsEmpty())
			{
				FBlueprintEditorUtils::RemoveBlueprintVariableMetaData(Blueprint, VarName, nullptr, FBlueprintMetadata::MD_Tooltip);
			}
			else
			{
				FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VarName, nullptr, FBlueprintMetadata::MD_Tooltip, Tooltip);
			}
		}
		if (bHasAdvancedDisplay)
		{
			FBlueprintEditorUtils::SetVariableAdvancedDisplayFlag(Blueprint, VarName, bAdvancedDisplay);
		}
		if (bHasPrivate)
		{
			if (bPrivate)
			{
				FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VarName, nullptr, FBlueprintMetadata::MD_Private, TEXT("true"));
			}
			else
			{
				FBlueprintEditorUtils::RemoveBlueprintVariableMetaData(Blueprint, VarName, nullptr, FBlueprintMetadata::MD_Private);
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetObjectField(TEXT("variable"), BuildVariableJson(Blueprint, Blueprint->NewVariables[VariableIndex]));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleSetVariableInstanceEditable(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString VariableName;
	bool bInstanceEditable = false;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("variable_name"), VariableName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'variable_name'"));
	}
	if (!Request.Params->TryGetBoolField(TEXT("instance_editable"), bInstanceEditable))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'instance_editable'"));
	}

	auto Task = [BlueprintPath, VariableName, bInstanceEditable]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return Result;
		}

		const FName VarName(*VariableName);
		const int32 VariableIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName);
		if (VariableIndex == INDEX_NONE)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Variable not found: %s"), *VariableName));
			return Result;
		}

		FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, VarName, !bInstanceEditable);
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetObjectField(TEXT("variable"), BuildVariableJson(Blueprint, Blueprint->NewVariables[VariableIndex]));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleSetVariableExposeOnSpawn(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString VariableName;
	bool bExposeOnSpawn = false;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("variable_name"), VariableName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'variable_name'"));
	}
	if (!Request.Params->TryGetBoolField(TEXT("expose_on_spawn"), bExposeOnSpawn))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'expose_on_spawn'"));
	}

	auto Task = [BlueprintPath, VariableName, bExposeOnSpawn]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return Result;
		}

		const FName VarName(*VariableName);
		const int32 VariableIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName);
		if (VariableIndex == INDEX_NONE)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Variable not found: %s"), *VariableName));
			return Result;
		}

		if (bExposeOnSpawn)
		{
			FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VarName, nullptr, FBlueprintMetadata::MD_ExposeOnSpawn, TEXT("true"));
		}
		else
		{
			FBlueprintEditorUtils::RemoveBlueprintVariableMetaData(Blueprint, VarName, nullptr, FBlueprintMetadata::MD_ExposeOnSpawn);
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetObjectField(TEXT("variable"), BuildVariableJson(Blueprint, Blueprint->NewVariables[VariableIndex]));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleSetVariableSaveGame(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString VariableName;
	bool bSaveGame = false;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("variable_name"), VariableName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'variable_name'"));
	}
	if (!Request.Params->TryGetBoolField(TEXT("save_game"), bSaveGame))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'save_game'"));
	}

	auto Task = [BlueprintPath, VariableName, bSaveGame]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return Result;
		}

		const FName VarName(*VariableName);
		const int32 VariableIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName);
		if (VariableIndex == INDEX_NONE)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Variable not found: %s"), *VariableName));
			return Result;
		}

		FBlueprintEditorUtils::SetVariableSaveGameFlag(Blueprint, VarName, bSaveGame);
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetObjectField(TEXT("variable"), BuildVariableJson(Blueprint, Blueprint->NewVariables[VariableIndex]));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleSetVariableTransient(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString VariableName;
	bool bTransient = false;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("variable_name"), VariableName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'variable_name'"));
	}
	if (!Request.Params->TryGetBoolField(TEXT("transient"), bTransient))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'transient'"));
	}

	auto Task = [BlueprintPath, VariableName, bTransient]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return Result;
		}

		const FName VarName(*VariableName);
		const int32 VariableIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName);
		if (VariableIndex == INDEX_NONE)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Variable not found: %s"), *VariableName));
			return Result;
		}

		FBlueprintEditorUtils::SetVariableTransientFlag(Blueprint, VarName, bTransient);
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetObjectField(TEXT("variable"), BuildVariableJson(Blueprint, Blueprint->NewVariables[VariableIndex]));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleSetVariableReplication(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString VariableName;
	bool bReplicated = false;
	FString RepNotifyFunctionName;
	FString ReplicationConditionName;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("variable_name"), VariableName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'variable_name'"));
	}

	const bool bHasReplicated = Request.Params->TryGetBoolField(TEXT("replicated"), bReplicated);
	const bool bHasRepNotifyFunction = Request.Params->TryGetStringField(TEXT("rep_notify_function"), RepNotifyFunctionName);
	const bool bHasReplicationCondition = Request.Params->TryGetStringField(TEXT("replication_condition"), ReplicationConditionName);
	if (!bHasReplicated && !bHasRepNotifyFunction && !bHasReplicationCondition)
	{
		return InvalidParams(Request.Id, TEXT("At least one of 'replicated', 'rep_notify_function', or 'replication_condition' is required"));
	}
	if (bHasReplicated && !bReplicated && bHasRepNotifyFunction && !RepNotifyFunctionName.TrimStartAndEnd().IsEmpty())
	{
		return InvalidParams(Request.Id, TEXT("rep_notify_function requires replicated=true"));
	}

	ELifetimeCondition ReplicationCondition = COND_None;
	if (bHasReplicationCondition)
	{
		FString ParseConditionError;
		if (!ParseReplicationCondition(ReplicationConditionName, ReplicationCondition, ParseConditionError))
		{
			return InvalidParams(Request.Id, ParseConditionError);
		}
	}

	auto Task = [BlueprintPath, VariableName, bHasReplicated, bReplicated, bHasRepNotifyFunction, RepNotifyFunctionName, bHasReplicationCondition, ReplicationCondition]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return Result;
		}

		const FName VarName(*VariableName);
		const int32 VariableIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName);
		if (VariableIndex == INDEX_NONE)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Variable not found: %s"), *VariableName));
			return Result;
		}

		uint64* PropertyFlags = FBlueprintEditorUtils::GetBlueprintVariablePropertyFlags(Blueprint, VarName);
		if (!PropertyFlags)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to resolve property flags for variable: %s"), *VariableName));
			return Result;
		}

		Blueprint->Modify();
		if (bHasReplicated)
		{
			if (bReplicated)
			{
				*PropertyFlags |= CPF_Net;
			}
			else
			{
				*PropertyFlags &= ~CPF_Net;
			}
		}

		if (bHasRepNotifyFunction)
		{
			const FString TrimmedRepNotifyFunctionName = RepNotifyFunctionName.TrimStartAndEnd();
			if (TrimmedRepNotifyFunctionName.IsEmpty())
			{
				FBlueprintEditorUtils::SetBlueprintVariableRepNotifyFunc(Blueprint, VarName, NAME_None);
				*PropertyFlags &= ~CPF_RepNotify;
			}
			else
			{
				FBlueprintEditorUtils::SetBlueprintVariableRepNotifyFunc(Blueprint, VarName, FName(*TrimmedRepNotifyFunctionName));
				*PropertyFlags |= CPF_RepNotify;
				*PropertyFlags |= CPF_Net;
			}
		}

		if (bHasReplicationCondition)
		{
			Blueprint->NewVariables[VariableIndex].ReplicationCondition = ReplicationCondition;
			*PropertyFlags |= CPF_Net;
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetObjectField(TEXT("variable"), BuildVariableJson(Blueprint, Blueprint->NewVariables[VariableIndex]));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleAddEventNode(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString EventName;
	FString EventClassPath = TEXT("/Script/Engine.Actor");
	FString GraphName = TEXT("EventGraph");
	int32 NodeX = 0;
	int32 NodeY = 0;

	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("event_name"), EventName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'event_name'"));
	}
	Request.Params->TryGetStringField(TEXT("event_class"), EventClassPath);
	Request.Params->TryGetStringField(TEXT("graph_name"), GraphName);
	Request.Params->TryGetNumberField(TEXT("x"), NodeX);
	Request.Params->TryGetNumberField(TEXT("y"), NodeY);

	auto Task = [BlueprintPath, EventName, EventClassPath, GraphName, NodeX, NodeY]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return Result;
		}

		UEdGraph* Graph = ResolveGraph(Blueprint, GraphName);
		if (!Graph)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Graph not found: %s"), *GraphName));
			return Result;
		}

		UClass* EventClass = ResolveClass(EventClassPath);
		if (!EventClass)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Event class not found: %s"), *EventClassPath));
			return Result;
		}

		int32 MutableY = NodeY;
		UK2Node_Event* EventNode = FKismetEditorUtilities::AddDefaultEventNode(
			Blueprint,
			Graph,
			NormalizeEventName(EventName),
			EventClass,
			MutableY
		);

		if (!EventNode)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to add event node: %s"), *EventName));
			return Result;
		}

		EventNode->NodePosX = NodeX;
		EventNode->NodePosY = NodeY;
		EventNode->ReconstructNode();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetObjectField(TEXT("node"), BuildNodeJson(EventNode));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleAddCallFunctionNode(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString FunctionClassPath;
	FString FunctionName;
	FString GraphName = TEXT("EventGraph");
	int32 NodeX = 0;
	int32 NodeY = 0;

	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("function_class"), FunctionClassPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'function_class'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("function_name"), FunctionName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'function_name'"));
	}
	Request.Params->TryGetStringField(TEXT("graph_name"), GraphName);
	Request.Params->TryGetNumberField(TEXT("x"), NodeX);
	Request.Params->TryGetNumberField(TEXT("y"), NodeY);

	auto Task = [BlueprintPath, FunctionClassPath, FunctionName, GraphName, NodeX, NodeY]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return Result;
		}

		UEdGraph* Graph = ResolveGraph(Blueprint, GraphName);
		if (!Graph)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Graph not found: %s"), *GraphName));
			return Result;
		}

		UClass* FunctionClass = ResolveClass(FunctionClassPath);
		if (!FunctionClass)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Function class not found: %s"), *FunctionClassPath));
			return Result;
		}

		UFunction* Function = FunctionClass->FindFunctionByName(FName(*FunctionName));
		if (!Function)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Function not found: %s::%s"), *FunctionClass->GetName(), *FunctionName));
			return Result;
		}

		FGraphNodeCreator<UK2Node_CallFunction> NodeCreator(*Graph);
		UK2Node_CallFunction* CallNode = NodeCreator.CreateNode();
		CallNode->SetFromFunction(Function);
		CallNode->NodePosX = NodeX;
		CallNode->NodePosY = NodeY;
		NodeCreator.Finalize();
		CallNode->ReconstructNode();

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetObjectField(TEXT("node"), BuildNodeJson(CallNode));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleAddVariableGetNode(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString VariableName;
	FString GraphName = TEXT("EventGraph");
	int32 NodeX = 0;
	int32 NodeY = 0;

	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("variable_name"), VariableName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'variable_name'"));
	}
	Request.Params->TryGetStringField(TEXT("graph_name"), GraphName);
	Request.Params->TryGetNumberField(TEXT("x"), NodeX);
	Request.Params->TryGetNumberField(TEXT("y"), NodeY);

	auto Task = [BlueprintPath, VariableName, GraphName, NodeX, NodeY]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return Result;
		}

		UEdGraph* Graph = ResolveGraph(Blueprint, GraphName);
		if (!Graph)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Graph not found: %s"), *GraphName));
			return Result;
		}

		FGraphNodeCreator<UK2Node_VariableGet> NodeCreator(*Graph);
		UK2Node_VariableGet* GetNode = NodeCreator.CreateNode();
		GetNode->VariableReference.SetSelfMember(FName(*VariableName));
		GetNode->NodePosX = NodeX;
		GetNode->NodePosY = NodeY;
		NodeCreator.Finalize();
		GetNode->ReconstructNode();

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetObjectField(TEXT("node"), BuildNodeJson(GetNode));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleAddVariableSetNode(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString VariableName;
	FString GraphName = TEXT("EventGraph");
	int32 NodeX = 0;
	int32 NodeY = 0;

	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("variable_name"), VariableName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'variable_name'"));
	}
	Request.Params->TryGetStringField(TEXT("graph_name"), GraphName);
	Request.Params->TryGetNumberField(TEXT("x"), NodeX);
	Request.Params->TryGetNumberField(TEXT("y"), NodeY);

	auto Task = [BlueprintPath, VariableName, GraphName, NodeX, NodeY]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return Result;
		}

		if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(*VariableName)) == INDEX_NONE)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Variable not found: %s"), *VariableName));
			return Result;
		}

		UEdGraph* Graph = ResolveGraph(Blueprint, GraphName);
		if (!Graph)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Graph not found: %s"), *GraphName));
			return Result;
		}

		FGraphNodeCreator<UK2Node_VariableSet> NodeCreator(*Graph);
		UK2Node_VariableSet* SetNode = NodeCreator.CreateNode();
		SetNode->VariableReference.SetSelfMember(FName(*VariableName));
		SetNode->NodePosX = NodeX;
		SetNode->NodePosY = NodeY;
		NodeCreator.Finalize();
		SetNode->ReconstructNode();

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetObjectField(TEXT("node"), BuildNodeJson(SetNode));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleSetPinDefaultValue(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString GraphName = TEXT("EventGraph");
	FString NodeId;
	FString PinName;
	FString DefaultValue;

	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("node_id"), NodeId))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'node_id'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("pin_name"), PinName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'pin_name'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("default_value"), DefaultValue))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'default_value'"));
	}
	Request.Params->TryGetStringField(TEXT("graph_name"), GraphName);

	auto Task = [BlueprintPath, GraphName, NodeId, PinName, DefaultValue]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return Result;
		}

		UEdGraph* Graph = ResolveGraph(Blueprint, GraphName);
		if (!Graph)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Graph not found: %s"), *GraphName));
			return Result;
		}

		UEdGraphNode* Node = FindNodeById(Graph, NodeId);
		if (!Node)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Could not resolve node by node_id"));
			return Result;
		}

		UEdGraphPin* Pin = FindPinByName(Node, PinName);
		if (!Pin)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Could not resolve pin by name"));
			return Result;
		}

		bool bSet = false;
		const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
		if (K2Schema)
		{
			K2Schema->TrySetDefaultValue(*Pin, DefaultValue);
			bSet = true;
		}

		if (!bSet)
		{
			Pin->Modify();
			Pin->DefaultValue = DefaultValue;
			bSet = true;
		}

		Node->PinDefaultValueChanged(Pin);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		Result->SetBoolField(TEXT("success"), bSet);
		Result->SetStringField(TEXT("node_id"), NodeId);
		Result->SetStringField(TEXT("pin_name"), PinName);
		Result->SetStringField(TEXT("default_value"), Pin->GetDefaultAsString());
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleConnectPins(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString GraphName = TEXT("EventGraph");
	FString FromNodeId;
	FString FromPinName;
	FString ToNodeId;
	FString ToPinName;

	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("from_node_id"), FromNodeId))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'from_node_id'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("from_pin"), FromPinName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'from_pin'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("to_node_id"), ToNodeId))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'to_node_id'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("to_pin"), ToPinName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'to_pin'"));
	}
	Request.Params->TryGetStringField(TEXT("graph_name"), GraphName);

	auto Task = [BlueprintPath, GraphName, FromNodeId, FromPinName, ToNodeId, ToPinName]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return Result;
		}

		UEdGraph* Graph = ResolveGraph(Blueprint, GraphName);
		if (!Graph)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Graph not found: %s"), *GraphName));
			return Result;
		}

		UEdGraphNode* FromNode = FindNodeById(Graph, FromNodeId);
		UEdGraphNode* ToNode = FindNodeById(Graph, ToNodeId);
		if (!FromNode || !ToNode)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Could not resolve from/to node by node_id"));
			return Result;
		}

		UEdGraphPin* FromPin = FindPinByName(FromNode, FromPinName);
		UEdGraphPin* ToPin = FindPinByName(ToNode, ToPinName);
		if (!FromPin || !ToPin)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Could not resolve from/to pin by name"));
			return Result;
		}

		const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
		if (!K2Schema)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("K2 schema unavailable"));
			return Result;
		}

		bool bConnected = K2Schema->TryCreateConnection(FromPin, ToPin);
		if (!bConnected)
		{
			bConnected = K2Schema->TryCreateConnection(ToPin, FromPin);
		}

		if (!bConnected)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Pins are not compatible or connection failed"));
			return Result;
		}

		FromNode->NodeConnectionListChanged();
		ToNode->NodeConnectionListChanged();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("from_node_id"), FromNodeId);
		Result->SetStringField(TEXT("from_pin"), FromPinName);
		Result->SetStringField(TEXT("to_node_id"), ToNodeId);
		Result->SetStringField(TEXT("to_pin"), ToPinName);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleCompileBlueprint(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}

	auto Task = [BlueprintPath]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return Result;
		}

		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, nullptr);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetNumberField(TEXT("status"), static_cast<int32>(Blueprint->Status));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}
