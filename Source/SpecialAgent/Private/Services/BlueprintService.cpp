// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/BlueprintService.h"

#include "GameThreadDispatcher.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BlueprintEditorLibrary.h"
#include "EditorAssetLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraphUtilities.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node.h"
#include "K2Node_Knot.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphSchema_K2_Actions.h"
#include "EdGraphNode_Comment.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/EngineTypes.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
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

	static FString PinDirectionToString(const EEdGraphPinDirection Direction)
	{
		switch (Direction)
		{
			case EGPD_Input:
				return TEXT("input");
			case EGPD_Output:
				return TEXT("output");
			default:
				return TEXT("unknown");
		}
	}

	static FString BuildPinPath(const UEdGraphPin* Pin)
	{
		if (!Pin)
		{
			return FString();
		}
		if (Pin->ParentPin)
		{
			return FString::Printf(TEXT("%s.%s"), *BuildPinPath(Pin->ParentPin), *Pin->PinName.ToString());
		}
		return Pin->PinName.ToString();
	}

	static void GatherPinRecursive(const UEdGraphPin* Pin, TArray<const UEdGraphPin*>& OutPins, TSet<const UEdGraphPin*>& SeenPins)
	{
		if (!Pin || SeenPins.Contains(Pin))
		{
			return;
		}

		SeenPins.Add(Pin);
		OutPins.Add(Pin);
		for (const UEdGraphPin* SubPin : Pin->SubPins)
		{
			GatherPinRecursive(SubPin, OutPins, SeenPins);
		}
	}

	static TArray<const UEdGraphPin*> GatherNodePins(const UEdGraphNode* Node)
	{
		TArray<const UEdGraphPin*> Pins;
		if (!Node)
		{
			return Pins;
		}

		TSet<const UEdGraphPin*> SeenPins;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->ParentPin)
			{
				continue;
			}
			GatherPinRecursive(Pin, Pins, SeenPins);
		}

		// Fallback: include any pins not reached through top-level traversal.
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && !SeenPins.Contains(Pin))
			{
				SeenPins.Add(Pin);
				Pins.Add(Pin);
			}
		}

		return Pins;
	}

	static UEdGraphPin* FindPinByPathOrName(UEdGraphNode* Node, const FString& PinIdentifier)
	{
		if (!Node)
		{
			return nullptr;
		}

		const FString TrimmedIdentifier = PinIdentifier.TrimStartAndEnd();
		if (TrimmedIdentifier.IsEmpty())
		{
			return nullptr;
		}

		const TArray<const UEdGraphPin*> AllPins = GatherNodePins(Node);
		for (const UEdGraphPin* Pin : AllPins)
		{
			if (!Pin)
			{
				continue;
			}
			if (BuildPinPath(Pin).Equals(TrimmedIdentifier, ESearchCase::CaseSensitive))
			{
				return const_cast<UEdGraphPin*>(Pin);
			}
		}

		for (const UEdGraphPin* Pin : AllPins)
		{
			if (!Pin)
			{
				continue;
			}
			if (BuildPinPath(Pin).Equals(TrimmedIdentifier, ESearchCase::IgnoreCase))
			{
				return const_cast<UEdGraphPin*>(Pin);
			}
		}

		for (const UEdGraphPin* Pin : AllPins)
		{
			if (!Pin)
			{
				continue;
			}
			if (Pin->PinName.ToString().Equals(TrimmedIdentifier, ESearchCase::CaseSensitive))
			{
				return const_cast<UEdGraphPin*>(Pin);
			}
		}

		for (const UEdGraphPin* Pin : AllPins)
		{
			if (!Pin)
			{
				continue;
			}
			if (Pin->PinName.ToString().Equals(TrimmedIdentifier, ESearchCase::IgnoreCase))
			{
				return const_cast<UEdGraphPin*>(Pin);
			}
		}

		return nullptr;
	}

	static TSharedPtr<FJsonObject> BuildPinJsonDetailed(const UEdGraphPin* Pin)
	{
		TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
		if (!Pin)
		{
			PinObj->SetStringField(TEXT("pin_name"), TEXT(""));
			return PinObj;
		}

		PinObj->SetStringField(TEXT("pin_name"), Pin->PinName.ToString());
		PinObj->SetStringField(TEXT("pin_path"), BuildPinPath(Pin));
		PinObj->SetStringField(TEXT("direction"), PinDirectionToString(Pin->Direction));
		PinObj->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
		PinObj->SetStringField(TEXT("container_type"), PinContainerTypeToString(Pin->PinType.ContainerType));
		PinObj->SetBoolField(TEXT("is_const"), Pin->PinType.bIsConst);
		PinObj->SetBoolField(TEXT("is_reference"), Pin->PinType.bIsReference);
		PinObj->SetBoolField(TEXT("is_linked"), Pin->LinkedTo.Num() > 0);
		PinObj->SetBoolField(TEXT("is_split_parent"), Pin->SubPins.Num() > 0);
		PinObj->SetBoolField(TEXT("is_split_child"), Pin->ParentPin != nullptr);
		PinObj->SetBoolField(TEXT("is_orphaned"), Pin->bOrphanedPin);
		PinObj->SetStringField(TEXT("default_value"), Pin->GetDefaultAsString());

		if (Pin->PinType.PinSubCategory != NAME_None)
		{
			PinObj->SetStringField(TEXT("subcategory"), Pin->PinType.PinSubCategory.ToString());
		}
		if (Pin->PinType.PinSubCategoryObject.IsValid())
		{
			PinObj->SetStringField(TEXT("subcategory_object"), Pin->PinType.PinSubCategoryObject->GetPathName());
		}
		if (Pin->ParentPin)
		{
			PinObj->SetStringField(TEXT("parent_pin_path"), BuildPinPath(Pin->ParentPin));
		}

		TArray<TSharedPtr<FJsonValue>> SubPinsJson;
		for (const UEdGraphPin* SubPin : Pin->SubPins)
		{
			if (!SubPin)
			{
				continue;
			}
			SubPinsJson.Add(MakeShared<FJsonValueString>(BuildPinPath(SubPin)));
		}
		PinObj->SetArrayField(TEXT("sub_pins"), SubPinsJson);

		TArray<TSharedPtr<FJsonValue>> LinkedToJson;
		for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			if (!LinkedPin || !LinkedPin->GetOwningNodeUnchecked())
			{
				continue;
			}

			const UEdGraphNode* LinkedNode = LinkedPin->GetOwningNodeUnchecked();
			TSharedPtr<FJsonObject> LinkObj = MakeShared<FJsonObject>();
			LinkObj->SetStringField(TEXT("node_id"), LinkedNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			LinkObj->SetStringField(TEXT("node_title"), LinkedNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
			LinkObj->SetStringField(TEXT("pin_name"), LinkedPin->PinName.ToString());
			LinkObj->SetStringField(TEXT("pin_path"), BuildPinPath(LinkedPin));
			LinkObj->SetStringField(TEXT("direction"), PinDirectionToString(LinkedPin->Direction));
			LinkedToJson.Add(MakeShared<FJsonValueObject>(LinkObj));
		}
		PinObj->SetArrayField(TEXT("linked_to"), LinkedToJson);
		return PinObj;
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

	static TArray<TSharedPtr<FJsonValue>> BuildVectorJsonArray(const FVector& VectorValue)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		Array.Reserve(3);
		Array.Add(MakeShared<FJsonValueNumber>(VectorValue.X));
		Array.Add(MakeShared<FJsonValueNumber>(VectorValue.Y));
		Array.Add(MakeShared<FJsonValueNumber>(VectorValue.Z));
		return Array;
	}

	static TArray<TSharedPtr<FJsonValue>> BuildRotatorJsonArray(const FRotator& RotatorValue)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		Array.Reserve(3);
		Array.Add(MakeShared<FJsonValueNumber>(RotatorValue.Pitch));
		Array.Add(MakeShared<FJsonValueNumber>(RotatorValue.Yaw));
		Array.Add(MakeShared<FJsonValueNumber>(RotatorValue.Roll));
		return Array;
	}

	static bool ParseVectorParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FVector& OutVector, FString& OutError, bool& bOutWasProvided)
	{
		bOutWasProvided = false;
		if (!Params.IsValid())
		{
			OutError = TEXT("Missing params object");
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* ValueArray = nullptr;
		if (!Params->TryGetArrayField(FieldName, ValueArray))
		{
			return true;
		}

		bOutWasProvided = true;
		if (!ValueArray || ValueArray->Num() != 3)
		{
			OutError = FString::Printf(TEXT("Parameter '%s' must be an array [X, Y, Z]"), FieldName);
			return false;
		}

		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
		if (!(*ValueArray)[0]->TryGetNumber(X) || !(*ValueArray)[1]->TryGetNumber(Y) || !(*ValueArray)[2]->TryGetNumber(Z))
		{
			OutError = FString::Printf(TEXT("Parameter '%s' must contain numeric values"), FieldName);
			return false;
		}

		OutVector = FVector(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
		return true;
	}

	static bool ParseRotatorParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FRotator& OutRotator, FString& OutError, bool& bOutWasProvided)
	{
		bOutWasProvided = false;
		if (!Params.IsValid())
		{
			OutError = TEXT("Missing params object");
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* ValueArray = nullptr;
		if (!Params->TryGetArrayField(FieldName, ValueArray))
		{
			return true;
		}

		bOutWasProvided = true;
		if (!ValueArray || ValueArray->Num() != 3)
		{
			OutError = FString::Printf(TEXT("Parameter '%s' must be an array [Pitch, Yaw, Roll]"), FieldName);
			return false;
		}

		double Pitch = 0.0;
		double Yaw = 0.0;
		double Roll = 0.0;
		if (!(*ValueArray)[0]->TryGetNumber(Pitch) || !(*ValueArray)[1]->TryGetNumber(Yaw) || !(*ValueArray)[2]->TryGetNumber(Roll))
		{
			OutError = FString::Printf(TEXT("Parameter '%s' must contain numeric values"), FieldName);
			return false;
		}

		OutRotator = FRotator(static_cast<float>(Pitch), static_cast<float>(Yaw), static_cast<float>(Roll));
		return true;
	}

	static FProperty* FindPropertyByNameCaseInsensitive(UClass* OwnerClass, const FString& PropertyName)
	{
		if (!OwnerClass || PropertyName.IsEmpty())
		{
			return nullptr;
		}

		const FName PropertyFName(*PropertyName);
		if (FProperty* ExactMatch = OwnerClass->FindPropertyByName(PropertyFName))
		{
			return ExactMatch;
		}

		for (TFieldIterator<FProperty> It(OwnerClass, EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (Property && Property->GetName().Equals(PropertyName, ESearchCase::IgnoreCase))
			{
				return Property;
			}
		}

		return nullptr;
	}

	static TSharedPtr<FJsonObject> BuildComponentJson(UBlueprint* Blueprint, USimpleConstructionScript* SCS, USCS_Node* Node)
	{
		TSharedPtr<FJsonObject> ComponentObj = MakeShared<FJsonObject>();
		if (!Node)
		{
			ComponentObj->SetStringField(TEXT("component_name"), TEXT("None"));
			return ComponentObj;
		}

		ComponentObj->SetStringField(TEXT("component_name"), Node->GetVariableName().ToString());
		ComponentObj->SetStringField(TEXT("guid"), Node->VariableGuid.ToString(EGuidFormats::DigitsWithHyphens));
		ComponentObj->SetStringField(TEXT("attach_socket"), Node->AttachToName.IsNone() ? TEXT("") : Node->AttachToName.ToString());
		ComponentObj->SetBoolField(TEXT("is_root"), Node->IsRootNode());
		ComponentObj->SetStringField(TEXT("parent_component_name"), Node->ParentComponentOrVariableName.IsNone() ? TEXT("") : Node->ParentComponentOrVariableName.ToString());

		UClass* ComponentClass = Node->ComponentClass;
		if (!ComponentClass && Node->ComponentTemplate)
		{
			ComponentClass = Node->ComponentTemplate->GetClass();
		}
		ComponentObj->SetStringField(TEXT("component_class"), ComponentClass ? ComponentClass->GetPathName() : TEXT(""));

		if (Node->ComponentTemplate)
		{
			ComponentObj->SetStringField(TEXT("template_name"), Node->ComponentTemplate->GetName());
			ComponentObj->SetStringField(TEXT("template_path"), Node->ComponentTemplate->GetPathName());
		}

		TArray<TSharedPtr<FJsonValue>> ChildrenJson;
		for (USCS_Node* ChildNode : Node->GetChildNodes())
		{
			if (!ChildNode)
			{
				continue;
			}
			ChildrenJson.Add(MakeShared<FJsonValueString>(ChildNode->GetVariableName().ToString()));
		}
		ComponentObj->SetArrayField(TEXT("children"), ChildrenJson);
		ComponentObj->SetNumberField(TEXT("child_count"), ChildrenJson.Num());

		if (USceneComponent* SceneTemplate = Cast<USceneComponent>(Node->ComponentTemplate))
		{
			ComponentObj->SetBoolField(TEXT("is_scene_component"), true);
			ComponentObj->SetArrayField(TEXT("relative_location"), BuildVectorJsonArray(SceneTemplate->GetRelativeLocation()));
			ComponentObj->SetArrayField(TEXT("relative_rotation"), BuildRotatorJsonArray(SceneTemplate->GetRelativeRotation()));
			ComponentObj->SetArrayField(TEXT("relative_scale"), BuildVectorJsonArray(SceneTemplate->GetRelativeScale3D()));
		}
		else
		{
			ComponentObj->SetBoolField(TEXT("is_scene_component"), false);
		}

		if (Blueprint && SCS)
		{
			USCS_Node* ParentNode = SCS->FindParentNode(Node);
			if (ParentNode)
			{
				ComponentObj->SetStringField(TEXT("parent_component_name"), ParentNode->GetVariableName().ToString());
			}
		}

		return ComponentObj;
	}

	enum class EFunctionParamDirection : uint8
	{
		Input,
		Output
	};

	static FString FunctionParamDirectionToString(const EFunctionParamDirection Direction)
	{
		return Direction == EFunctionParamDirection::Output ? TEXT("output") : TEXT("input");
	}

	static bool ParseFunctionParamDirection(const FString& DirectionName, EFunctionParamDirection& OutDirection, FString& OutError)
	{
		const FString Normalized = DirectionName.TrimStartAndEnd().ToLower();
		if (Normalized.IsEmpty() || Normalized == TEXT("input") || Normalized == TEXT("in"))
		{
			OutDirection = EFunctionParamDirection::Input;
			return true;
		}
		if (Normalized == TEXT("output") || Normalized == TEXT("out"))
		{
			OutDirection = EFunctionParamDirection::Output;
			return true;
		}

		OutError = FString::Printf(TEXT("Unsupported direction '%s'. Supported: input, output"), *DirectionName);
		return false;
	}

	static bool ResolvePinTypeFromTypeSpec(
		const FString& RequestedType,
		const FString& TypeReferencePath,
		const bool bHasContainerTypeOverride,
		const EPinContainerType ContainerTypeOverride,
		const bool bHasMapKeyType,
		const FString& MapKeyTypeName,
		const FString& MapKeyTypeReferencePath,
		FEdGraphPinType& OutPinType,
		FString& OutError
	)
	{
		FString ParsedValueTypeName;
		FString ParsedMapKeyTypeName;
		EPinContainerType ParsedContainerType = EPinContainerType::None;
		if (!ParseContainerFromVariableType(RequestedType, ParsedValueTypeName, ParsedContainerType, ParsedMapKeyTypeName, OutError))
		{
			return false;
		}

		const EPinContainerType FinalContainerType = bHasContainerTypeOverride ? ContainerTypeOverride : ParsedContainerType;
		const FString ValueTypeName = ParsedValueTypeName.TrimStartAndEnd();
		if (ValueTypeName.IsEmpty())
		{
			OutError = TEXT("Resolved type is empty");
			return false;
		}

		FEdGraphPinType ValuePinType;
		if (!BuildPinType(ValueTypeName, TypeReferencePath, ValuePinType, OutError))
		{
			return false;
		}
		if (ValuePinType.ContainerType != EPinContainerType::None)
		{
			OutError = TEXT("Nested container types are not supported");
			return false;
		}

		OutPinType = ValuePinType;
		OutPinType.ContainerType = FinalContainerType;
		OutPinType.PinValueType = FEdGraphTerminalType();

		if (FinalContainerType == EPinContainerType::Map)
		{
			const FString EffectiveMapKeyTypeName = bHasMapKeyType ? MapKeyTypeName.TrimStartAndEnd() : ParsedMapKeyTypeName.TrimStartAndEnd();
			if (EffectiveMapKeyTypeName.IsEmpty())
			{
				OutError = TEXT("Map types require map_key_type (or map<key_type,value_type> syntax)");
				return false;
			}

			FEdGraphPinType KeyPinType;
			if (!BuildPinType(EffectiveMapKeyTypeName, MapKeyTypeReferencePath, KeyPinType, OutError))
			{
				return false;
			}
			if (KeyPinType.ContainerType != EPinContainerType::None)
			{
				OutError = TEXT("Map key type cannot be a container");
				return false;
			}

			OutPinType.PinCategory = KeyPinType.PinCategory;
			OutPinType.PinSubCategory = KeyPinType.PinSubCategory;
			OutPinType.PinSubCategoryObject = KeyPinType.PinSubCategoryObject;
			OutPinType.PinSubCategoryMemberReference = KeyPinType.PinSubCategoryMemberReference;
			OutPinType.bIsReference = KeyPinType.bIsReference;
			OutPinType.bIsConst = KeyPinType.bIsConst;
			OutPinType.bIsWeakPointer = KeyPinType.bIsWeakPointer;
			OutPinType.bIsUObjectWrapper = KeyPinType.bIsUObjectWrapper;
			OutPinType.bSerializeAsSinglePrecisionFloat = KeyPinType.bSerializeAsSinglePrecisionFloat;

			OutPinType.PinValueType.TerminalCategory = ValuePinType.PinCategory;
			OutPinType.PinValueType.TerminalSubCategory = ValuePinType.PinSubCategory;
			OutPinType.PinValueType.TerminalSubCategoryObject = ValuePinType.PinSubCategoryObject;
			OutPinType.PinValueType.bTerminalIsConst = ValuePinType.bIsConst;
			OutPinType.PinValueType.bTerminalIsWeakPointer = ValuePinType.bIsWeakPointer;
			OutPinType.PinValueType.bTerminalIsUObjectWrapper = ValuePinType.bIsUObjectWrapper;
		}

		return true;
	}

	static TSharedPtr<FJsonObject> BuildPinTypeJson(const FEdGraphPinType& PinType)
	{
		TSharedPtr<FJsonObject> PinTypeObj = MakeShared<FJsonObject>();
		const FString BaseTypeName = BaseTypeNameFromPinType(PinType);

		PinTypeObj->SetStringField(TEXT("base_type"), BaseTypeName);
		PinTypeObj->SetStringField(TEXT("container_type"), PinContainerTypeToString(PinType.ContainerType));
		PinTypeObj->SetBoolField(TEXT("pass_by_reference"), PinType.bIsReference);
		PinTypeObj->SetBoolField(TEXT("const"), PinType.bIsConst);

		if (PinType.ContainerType == EPinContainerType::Map)
		{
			const FString KeyTypeName = BaseTypeName;
			const FString ValueTypeName = BaseTypeNameFromTerminalType(PinType.PinValueType);
			PinTypeObj->SetStringField(TEXT("map_key_type"), KeyTypeName);
			PinTypeObj->SetStringField(TEXT("map_value_type"), ValueTypeName);
			PinTypeObj->SetStringField(TEXT("param_type"), FString::Printf(TEXT("map<%s,%s>"), *KeyTypeName, *ValueTypeName));

			if (PinType.PinSubCategoryObject.IsValid())
			{
				PinTypeObj->SetStringField(TEXT("map_key_type_reference"), PinType.PinSubCategoryObject->GetPathName());
			}
			if (PinType.PinValueType.TerminalSubCategoryObject.IsValid())
			{
				PinTypeObj->SetStringField(TEXT("map_value_type_reference"), PinType.PinValueType.TerminalSubCategoryObject->GetPathName());
			}
		}
		else if (PinType.ContainerType == EPinContainerType::Array)
		{
			PinTypeObj->SetStringField(TEXT("param_type"), FString::Printf(TEXT("array<%s>"), *BaseTypeName));
		}
		else if (PinType.ContainerType == EPinContainerType::Set)
		{
			PinTypeObj->SetStringField(TEXT("param_type"), FString::Printf(TEXT("set<%s>"), *BaseTypeName));
		}
		else
		{
			PinTypeObj->SetStringField(TEXT("param_type"), BaseTypeName);
		}

		if (PinType.PinSubCategoryObject.IsValid())
		{
			PinTypeObj->SetStringField(TEXT("type_reference"), PinType.PinSubCategoryObject->GetPathName());
		}

		return PinTypeObj;
	}

	static TSharedPtr<FJsonObject> BuildUserPinJson(const FUserPinInfo& PinInfo, const EFunctionParamDirection Direction)
	{
		TSharedPtr<FJsonObject> ParamObj = BuildPinTypeJson(PinInfo.PinType);
		ParamObj->SetStringField(TEXT("param_name"), PinInfo.PinName.ToString());
		ParamObj->SetStringField(TEXT("direction"), FunctionParamDirectionToString(Direction));
		ParamObj->SetStringField(TEXT("default_value"), PinInfo.PinDefaultValue);
		return ParamObj;
	}

	static TSharedPtr<FJsonObject> BuildFunctionFlagsJson(const UK2Node_FunctionEntry* FunctionEntry)
	{
		TSharedPtr<FJsonObject> FlagsObj = MakeShared<FJsonObject>();
		if (!FunctionEntry)
		{
			FlagsObj->SetBoolField(TEXT("pure"), false);
			FlagsObj->SetBoolField(TEXT("const"), false);
			FlagsObj->SetBoolField(TEXT("call_in_editor"), false);
			FlagsObj->SetStringField(TEXT("access"), TEXT("public"));
			return FlagsObj;
		}

		const uint32 FunctionFlags = FunctionEntry->GetFunctionFlags();
		FlagsObj->SetBoolField(TEXT("pure"), (FunctionFlags & FUNC_BlueprintPure) != 0);
		FlagsObj->SetBoolField(TEXT("const"), (FunctionFlags & FUNC_Const) != 0);
		FlagsObj->SetBoolField(TEXT("call_in_editor"), FunctionEntry->MetaData.bCallInEditor);
		FlagsObj->SetStringField(TEXT("access"), AccessSpecifierToString(FunctionFlags & FUNC_AccessSpecifiers));
		return FlagsObj;
	}

	static TArray<UK2Node_EditablePinBase*> GatherAllResultNodes(UK2Node_EditablePinBase* TargetNode)
	{
		TArray<UK2Node_EditablePinBase*> ResultNodes;
		if (UK2Node_FunctionResult* FunctionResultNode = Cast<UK2Node_FunctionResult>(TargetNode))
		{
			for (UK2Node_FunctionResult* Node : FunctionResultNode->GetAllResultNodes())
			{
				if (Node)
				{
					ResultNodes.Add(Node);
				}
			}
		}
		else if (TargetNode)
		{
			ResultNodes.Add(TargetNode);
		}

		return ResultNodes;
	}

	static void RefreshEditablePinNode(UK2Node_EditablePinBase* Node)
	{
		if (!Node)
		{
			return;
		}

		const bool bDisableOrphanPinSaving = Node->bDisableOrphanPinSaving;
		Node->bDisableOrphanPinSaving = true;
		Node->ReconstructNode();
		Node->bDisableOrphanPinSaving = bDisableOrphanPinSaving;

		if (const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>())
		{
			K2Schema->HandleParameterDefaultValueChanged(Node);
		}
	}

	static UEdGraph* FindFunctionGraphByName(UBlueprint* Blueprint, const FString& FunctionName)
	{
		if (!Blueprint)
		{
			return nullptr;
		}

		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph && Graph->GetName().Equals(FunctionName, ESearchCase::IgnoreCase))
			{
				return Graph;
			}
		}

		return nullptr;
	}

	static UEdGraph* FindMacroGraphByName(UBlueprint* Blueprint, const FString& MacroName)
	{
		if (!Blueprint)
		{
			return nullptr;
		}

		for (UEdGraph* Graph : Blueprint->MacroGraphs)
		{
			if (Graph && Graph->GetName().Equals(MacroName, ESearchCase::IgnoreCase))
			{
				return Graph;
			}
		}

		return nullptr;
	}

	static int32 FindDispatcherVariableIndex(UBlueprint* Blueprint, const FName DispatcherName)
	{
		if (!Blueprint || DispatcherName.IsNone())
		{
			return INDEX_NONE;
		}

		const int32 VariableIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, DispatcherName);
		if (VariableIndex == INDEX_NONE)
		{
			return INDEX_NONE;
		}
		if (Blueprint->NewVariables[VariableIndex].VarType.PinCategory != UEdGraphSchema_K2::PC_MCDelegate)
		{
			return INDEX_NONE;
		}
		return VariableIndex;
	}

	static TSharedPtr<FJsonObject> BuildFunctionJson(UBlueprint* Blueprint, UEdGraph* FunctionGraph)
	{
		TSharedPtr<FJsonObject> FunctionObj = BuildGraphJson(Blueprint, FunctionGraph);
		if (!FunctionGraph)
		{
			return FunctionObj;
		}

		FunctionObj->SetStringField(TEXT("function_name"), FunctionGraph->GetName());

		UK2Node_FunctionEntry* FunctionEntry = Cast<UK2Node_FunctionEntry>(FBlueprintEditorUtils::GetEntryNode(FunctionGraph));
		FunctionObj->SetObjectField(TEXT("flags"), BuildFunctionFlagsJson(FunctionEntry));

		TArray<TSharedPtr<FJsonValue>> InputParamsJson;
		if (FunctionEntry)
		{
			for (const TSharedPtr<FUserPinInfo>& UserPin : FunctionEntry->UserDefinedPins)
			{
				if (UserPin.IsValid())
				{
					InputParamsJson.Add(MakeShared<FJsonValueObject>(BuildUserPinJson(*UserPin.Get(), EFunctionParamDirection::Input)));
				}
			}
		}
		FunctionObj->SetArrayField(TEXT("input_params"), InputParamsJson);

		TArray<UK2Node_FunctionResult*> ResultNodes;
		FunctionGraph->GetNodesOfClass(ResultNodes);
		UK2Node_FunctionResult* PrimaryResultNode = ResultNodes.Num() > 0 ? ResultNodes[0] : nullptr;

		TArray<TSharedPtr<FJsonValue>> OutputParamsJson;
		if (PrimaryResultNode)
		{
			for (const TSharedPtr<FUserPinInfo>& UserPin : PrimaryResultNode->UserDefinedPins)
			{
				if (UserPin.IsValid())
				{
					TSharedPtr<FJsonObject> PinJson = BuildUserPinJson(*UserPin.Get(), EFunctionParamDirection::Output);
					if (UserPin->PinName == UEdGraphSchema_K2::PN_ReturnValue)
					{
						FunctionObj->SetObjectField(TEXT("return_pin"), PinJson);
					}
					OutputParamsJson.Add(MakeShared<FJsonValueObject>(PinJson));
				}
			}
		}
		FunctionObj->SetArrayField(TEXT("output_params"), OutputParamsJson);
		return FunctionObj;
	}

	static TSharedPtr<FJsonObject> BuildDispatcherJson(UBlueprint* Blueprint, const FBPVariableDescription& Variable)
	{
		TSharedPtr<FJsonObject> DispatcherObj = MakeShared<FJsonObject>();
		DispatcherObj->SetStringField(TEXT("dispatcher_name"), Variable.VarName.ToString());
		DispatcherObj->SetObjectField(TEXT("variable"), BuildVariableJson(Blueprint, Variable));

		UEdGraph* SignatureGraph = FBlueprintEditorUtils::GetDelegateSignatureGraphByName(Blueprint, Variable.VarName);
		if (SignatureGraph)
		{
			DispatcherObj->SetStringField(TEXT("signature_graph_name"), SignatureGraph->GetName());
			DispatcherObj->SetStringField(TEXT("signature_graph_path"), SignatureGraph->GetPathName());
		}
		else
		{
			DispatcherObj->SetStringField(TEXT("signature_graph_name"), TEXT(""));
			DispatcherObj->SetStringField(TEXT("signature_graph_path"), TEXT(""));
		}

		TArray<TSharedPtr<FJsonValue>> SignatureParamsJson;
		if (UK2Node_FunctionEntry* EntryNode = SignatureGraph ? Cast<UK2Node_FunctionEntry>(FBlueprintEditorUtils::GetEntryNode(SignatureGraph)) : nullptr)
		{
			for (const TSharedPtr<FUserPinInfo>& UserPin : EntryNode->UserDefinedPins)
			{
				if (UserPin.IsValid())
				{
					SignatureParamsJson.Add(MakeShared<FJsonValueObject>(BuildUserPinJson(*UserPin.Get(), EFunctionParamDirection::Input)));
				}
			}
		}
		DispatcherObj->SetArrayField(TEXT("signature_params"), SignatureParamsJson);
		return DispatcherObj;
	}

	static bool ExtractStringArrayField(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		TArray<FString>& OutValues,
		FString& OutError
	)
	{
		OutValues.Reset();
		if (!Params.IsValid())
		{
			OutError = TEXT("Missing params object");
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(FieldName, Values))
		{
			OutError = FString::Printf(TEXT("Missing required parameter '%s'"), FieldName);
			return false;
		}
		if (!Values || Values->Num() == 0)
		{
			OutError = FString::Printf(TEXT("Parameter '%s' must be a non-empty string array"), FieldName);
			return false;
		}

		for (int32 Index = 0; Index < Values->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Value = (*Values)[Index];
			FString StringValue;
			if (!Value.IsValid() || !Value->TryGetString(StringValue))
			{
				OutError = FString::Printf(TEXT("Parameter '%s' index %d must be a string"), FieldName, Index);
				return false;
			}

			StringValue = StringValue.TrimStartAndEnd();
			if (StringValue.IsEmpty())
			{
				OutError = FString::Printf(TEXT("Parameter '%s' index %d must be non-empty"), FieldName, Index);
				return false;
			}

			OutValues.Add(StringValue);
		}

		return true;
	}

	static bool FindNodesByIds(
		UEdGraph* Graph,
		const TArray<FString>& NodeIds,
		TArray<UEdGraphNode*>& OutNodes,
		FString& OutError
	)
	{
		OutNodes.Reset();
		if (!Graph)
		{
			OutError = TEXT("Graph not found");
			return false;
		}

		TSet<FGuid> AddedGuids;
		for (const FString& NodeId : NodeIds)
		{
			FGuid NodeGuid;
			if (!FGuid::Parse(NodeId, NodeGuid))
			{
				OutError = FString::Printf(TEXT("Invalid node id format: %s"), *NodeId);
				return false;
			}

			if (AddedGuids.Contains(NodeGuid))
			{
				continue;
			}

			UEdGraphNode* ResolvedNode = nullptr;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && Node->NodeGuid == NodeGuid)
				{
					ResolvedNode = Node;
					break;
				}
			}

			if (!ResolvedNode)
			{
				OutError = FString::Printf(TEXT("Node not found for node_id: %s"), *NodeId);
				return false;
			}

			AddedGuids.Add(NodeGuid);
			OutNodes.Add(ResolvedNode);
		}

		return OutNodes.Num() > 0;
	}

	static FVector2D ComputeAverageNodeLocation(const TArray<UEdGraphNode*>& Nodes)
	{
		if (Nodes.Num() == 0)
		{
			return FVector2D::ZeroVector;
		}

		double SumX = 0.0;
		double SumY = 0.0;
		for (UEdGraphNode* Node : Nodes)
		{
			if (!Node)
			{
				continue;
			}
			SumX += static_cast<double>(Node->NodePosX);
			SumY += static_cast<double>(Node->NodePosY);
		}

		const double Count = static_cast<double>(Nodes.Num());
		return FVector2D(SumX / Count, SumY / Count);
	}

	static TArray<TSharedPtr<FJsonValue>> BuildNodesJsonArray(const TArray<UEdGraphNode*>& Nodes)
	{
		TArray<TSharedPtr<FJsonValue>> NodesJson;
		NodesJson.Reserve(Nodes.Num());
		for (UEdGraphNode* Node : Nodes)
		{
			if (!Node)
			{
				continue;
			}

			NodesJson.Add(MakeShared<FJsonValueObject>(BuildNodeJson(Node)));
		}
		return NodesJson;
	}

	static UEdGraphNode* SpawnNodeFromClass(UEdGraph* Graph, UClass* NodeClass, const int32 NodeX, const int32 NodeY)
	{
		if (!Graph || !NodeClass || !NodeClass->IsChildOf(UEdGraphNode::StaticClass()) || NodeClass->HasAnyClassFlags(CLASS_Abstract))
		{
			return nullptr;
		}

		UEdGraphNode* NewNode = NewObject<UEdGraphNode>(Graph, NodeClass);
		if (!NewNode)
		{
			return nullptr;
		}

		if (Graph->HasAnyFlags(RF_Transactional))
		{
			NewNode->SetFlags(RF_Transactional);
		}

		Graph->AddNode(NewNode, true, false);
		NewNode->CreateNewGuid();
		NewNode->PostPlacedNewNode();
		NewNode->AllocateDefaultPins();
		NewNode->NodePosX = NodeX;
		NewNode->NodePosY = NodeY;
		NewNode->AutowireNewNode(nullptr);
		return NewNode;
	}

	static UK2Node_CallFunction* SpawnSelfCallFunctionNode(UEdGraph* Graph, UBlueprint* Blueprint, const FName FunctionName, const FVector2D& NodePosition)
	{
		if (!Graph || !Blueprint || FunctionName.IsNone())
		{
			return nullptr;
		}

		UFunction* TargetFunction = nullptr;
		if (Blueprint->SkeletonGeneratedClass)
		{
			TargetFunction = FindUField<UFunction>(Blueprint->SkeletonGeneratedClass, FunctionName);
		}
		if (!TargetFunction && Blueprint->GeneratedClass)
		{
			TargetFunction = FindUField<UFunction>(Blueprint->GeneratedClass, FunctionName);
		}

		FGraphNodeCreator<UK2Node_CallFunction> CallCreator(*Graph);
		UK2Node_CallFunction* CallNode = CallCreator.CreateNode();
		if (!CallNode)
		{
			return nullptr;
		}

		if (TargetFunction)
		{
			CallNode->SetFromFunction(TargetFunction);
		}
		else
		{
			CallNode->FunctionReference.SetSelfMember(FunctionName);
		}

		CallNode->NodePosX = static_cast<int32>(NodePosition.X);
		CallNode->NodePosY = static_cast<int32>(NodePosition.Y);
		CallCreator.Finalize();
		CallNode->ReconstructNode();
		return CallNode;
	}

	static UK2Node_MacroInstance* SpawnMacroInstanceNode(UEdGraph* Graph, UEdGraph* MacroGraph, const FVector2D& NodePosition)
	{
		if (!Graph || !MacroGraph)
		{
			return nullptr;
		}

		FGraphNodeCreator<UK2Node_MacroInstance> MacroCreator(*Graph);
		UK2Node_MacroInstance* MacroNode = MacroCreator.CreateNode();
		if (!MacroNode)
		{
			return nullptr;
		}

		MacroNode->SetMacroGraph(MacroGraph);
		MacroNode->NodePosX = static_cast<int32>(NodePosition.X);
		MacroNode->NodePosY = static_cast<int32>(NodePosition.Y);
		MacroCreator.Finalize();
		MacroNode->ReconstructNode();
		return MacroNode;
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
		Tool.Name = TEXT("list_components");
		Tool.Description = TEXT("List Blueprint SCS components and hierarchy details.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("add_component");
		Tool.Description = TEXT("Add a component template to a Blueprint SCS.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> ClassParam = MakeShared<FJsonObject>();
		ClassParam->SetStringField(TEXT("type"), TEXT("string"));
		ClassParam->SetStringField(TEXT("description"), TEXT("Component class path or class name."));
		Tool.Parameters->SetObjectField(TEXT("component_class"), ClassParam);

		TSharedPtr<FJsonObject> NameParam = MakeShared<FJsonObject>();
		NameParam->SetStringField(TEXT("type"), TEXT("string"));
		NameParam->SetStringField(TEXT("description"), TEXT("Optional component variable name."));
		Tool.Parameters->SetObjectField(TEXT("component_name"), NameParam);

		TSharedPtr<FJsonObject> ParentParam = MakeShared<FJsonObject>();
		ParentParam->SetStringField(TEXT("type"), TEXT("string"));
		ParentParam->SetStringField(TEXT("description"), TEXT("Optional parent scene component variable name."));
		Tool.Parameters->SetObjectField(TEXT("parent_component_name"), ParentParam);

		TSharedPtr<FJsonObject> SocketParam = MakeShared<FJsonObject>();
		SocketParam->SetStringField(TEXT("type"), TEXT("string"));
		SocketParam->SetStringField(TEXT("description"), TEXT("Optional socket name for scene component attachment."));
		Tool.Parameters->SetObjectField(TEXT("socket_name"), SocketParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("component_class"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("remove_component");
		Tool.Description = TEXT("Remove a component from a Blueprint SCS.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> NameParam = MakeShared<FJsonObject>();
		NameParam->SetStringField(TEXT("type"), TEXT("string"));
		NameParam->SetStringField(TEXT("description"), TEXT("Component variable name to remove."));
		Tool.Parameters->SetObjectField(TEXT("component_name"), NameParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("component_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("rename_component");
		Tool.Description = TEXT("Rename a component variable in a Blueprint SCS.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> NameParam = MakeShared<FJsonObject>();
		NameParam->SetStringField(TEXT("type"), TEXT("string"));
		NameParam->SetStringField(TEXT("description"), TEXT("Existing component variable name."));
		Tool.Parameters->SetObjectField(TEXT("component_name"), NameParam);

		TSharedPtr<FJsonObject> NewNameParam = MakeShared<FJsonObject>();
		NewNameParam->SetStringField(TEXT("type"), TEXT("string"));
		NewNameParam->SetStringField(TEXT("description"), TEXT("New component variable name."));
		Tool.Parameters->SetObjectField(TEXT("new_component_name"), NewNameParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("component_name"));
		Tool.RequiredParams.Add(TEXT("new_component_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("set_root_component");
		Tool.Description = TEXT("Set a scene component as the Blueprint root component.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> NameParam = MakeShared<FJsonObject>();
		NameParam->SetStringField(TEXT("type"), TEXT("string"));
		NameParam->SetStringField(TEXT("description"), TEXT("Component variable name to set as root."));
		Tool.Parameters->SetObjectField(TEXT("component_name"), NameParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("component_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("attach_component");
		Tool.Description = TEXT("Attach a scene component to another scene component.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> NameParam = MakeShared<FJsonObject>();
		NameParam->SetStringField(TEXT("type"), TEXT("string"));
		NameParam->SetStringField(TEXT("description"), TEXT("Child component variable name."));
		Tool.Parameters->SetObjectField(TEXT("component_name"), NameParam);

		TSharedPtr<FJsonObject> ParentParam = MakeShared<FJsonObject>();
		ParentParam->SetStringField(TEXT("type"), TEXT("string"));
		ParentParam->SetStringField(TEXT("description"), TEXT("Parent component variable name."));
		Tool.Parameters->SetObjectField(TEXT("parent_component_name"), ParentParam);

		TSharedPtr<FJsonObject> SocketParam = MakeShared<FJsonObject>();
		SocketParam->SetStringField(TEXT("type"), TEXT("string"));
		SocketParam->SetStringField(TEXT("description"), TEXT("Optional socket name."));
		Tool.Parameters->SetObjectField(TEXT("socket_name"), SocketParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("component_name"));
		Tool.RequiredParams.Add(TEXT("parent_component_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("detach_component");
		Tool.Description = TEXT("Detach a component and promote it to a root-level SCS node.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> NameParam = MakeShared<FJsonObject>();
		NameParam->SetStringField(TEXT("type"), TEXT("string"));
		NameParam->SetStringField(TEXT("description"), TEXT("Component variable name."));
		Tool.Parameters->SetObjectField(TEXT("component_name"), NameParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("component_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("set_component_property");
		Tool.Description = TEXT("Set a component template property value using Unreal import-text format.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> NameParam = MakeShared<FJsonObject>();
		NameParam->SetStringField(TEXT("type"), TEXT("string"));
		NameParam->SetStringField(TEXT("description"), TEXT("Component variable name."));
		Tool.Parameters->SetObjectField(TEXT("component_name"), NameParam);

		TSharedPtr<FJsonObject> PropertyParam = MakeShared<FJsonObject>();
		PropertyParam->SetStringField(TEXT("type"), TEXT("string"));
		PropertyParam->SetStringField(TEXT("description"), TEXT("Property name on the component template."));
		Tool.Parameters->SetObjectField(TEXT("property_name"), PropertyParam);

		TSharedPtr<FJsonObject> ValueParam = MakeShared<FJsonObject>();
		ValueParam->SetStringField(TEXT("type"), TEXT("string"));
		ValueParam->SetStringField(TEXT("description"), TEXT("Property value string in Unreal text format."));
		Tool.Parameters->SetObjectField(TEXT("value"), ValueParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("component_name"));
		Tool.RequiredParams.Add(TEXT("property_name"));
		Tool.RequiredParams.Add(TEXT("value"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("get_component_property");
		Tool.Description = TEXT("Get a component template property value as Unreal export-text.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> NameParam = MakeShared<FJsonObject>();
		NameParam->SetStringField(TEXT("type"), TEXT("string"));
		NameParam->SetStringField(TEXT("description"), TEXT("Component variable name."));
		Tool.Parameters->SetObjectField(TEXT("component_name"), NameParam);

		TSharedPtr<FJsonObject> PropertyParam = MakeShared<FJsonObject>();
		PropertyParam->SetStringField(TEXT("type"), TEXT("string"));
		PropertyParam->SetStringField(TEXT("description"), TEXT("Property name on the component template."));
		Tool.Parameters->SetObjectField(TEXT("property_name"), PropertyParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("component_name"));
		Tool.RequiredParams.Add(TEXT("property_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("set_component_transform_default");
		Tool.Description = TEXT("Set default relative transform values on a scene component template.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> NameParam = MakeShared<FJsonObject>();
		NameParam->SetStringField(TEXT("type"), TEXT("string"));
		NameParam->SetStringField(TEXT("description"), TEXT("Scene component variable name."));
		Tool.Parameters->SetObjectField(TEXT("component_name"), NameParam);

		TSharedPtr<FJsonObject> LocationParam = MakeShared<FJsonObject>();
		LocationParam->SetStringField(TEXT("type"), TEXT("array"));
		LocationParam->SetStringField(TEXT("description"), TEXT("Optional location as [X, Y, Z]."));
		Tool.Parameters->SetObjectField(TEXT("location"), LocationParam);

		TSharedPtr<FJsonObject> RotationParam = MakeShared<FJsonObject>();
		RotationParam->SetStringField(TEXT("type"), TEXT("array"));
		RotationParam->SetStringField(TEXT("description"), TEXT("Optional rotation as [Pitch, Yaw, Roll]."));
		Tool.Parameters->SetObjectField(TEXT("rotation"), RotationParam);

		TSharedPtr<FJsonObject> ScaleParam = MakeShared<FJsonObject>();
		ScaleParam->SetStringField(TEXT("type"), TEXT("array"));
		ScaleParam->SetStringField(TEXT("description"), TEXT("Optional scale as [X, Y, Z]."));
		Tool.Parameters->SetObjectField(TEXT("scale"), ScaleParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("component_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("list_functions");
		Tool.Description = TEXT("List user-authored Blueprint functions with flags and signature pins.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("create_function");
		Tool.Description = TEXT("Create a new Blueprint function graph.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> NameParam = MakeShared<FJsonObject>();
		NameParam->SetStringField(TEXT("type"), TEXT("string"));
		NameParam->SetStringField(TEXT("description"), TEXT("Function name."));
		Tool.Parameters->SetObjectField(TEXT("function_name"), NameParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("function_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("delete_function");
		Tool.Description = TEXT("Delete a Blueprint function graph.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> NameParam = MakeShared<FJsonObject>();
		NameParam->SetStringField(TEXT("type"), TEXT("string"));
		NameParam->SetStringField(TEXT("description"), TEXT("Function name."));
		Tool.Parameters->SetObjectField(TEXT("function_name"), NameParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("function_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("rename_function");
		Tool.Description = TEXT("Rename a Blueprint function graph.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> NameParam = MakeShared<FJsonObject>();
		NameParam->SetStringField(TEXT("type"), TEXT("string"));
		NameParam->SetStringField(TEXT("description"), TEXT("Existing function name."));
		Tool.Parameters->SetObjectField(TEXT("function_name"), NameParam);

		TSharedPtr<FJsonObject> NewNameParam = MakeShared<FJsonObject>();
		NewNameParam->SetStringField(TEXT("type"), TEXT("string"));
		NewNameParam->SetStringField(TEXT("description"), TEXT("New function name."));
		Tool.Parameters->SetObjectField(TEXT("new_function_name"), NewNameParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("function_name"));
		Tool.RequiredParams.Add(TEXT("new_function_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("set_function_flags");
		Tool.Description = TEXT("Set function flags: pure, const, call_in_editor, and access (public/protected/private).");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> NameParam = MakeShared<FJsonObject>();
		NameParam->SetStringField(TEXT("type"), TEXT("string"));
		NameParam->SetStringField(TEXT("description"), TEXT("Function name."));
		Tool.Parameters->SetObjectField(TEXT("function_name"), NameParam);

		TSharedPtr<FJsonObject> PureParam = MakeShared<FJsonObject>();
		PureParam->SetStringField(TEXT("type"), TEXT("boolean"));
		PureParam->SetStringField(TEXT("description"), TEXT("Optional pure flag."));
		Tool.Parameters->SetObjectField(TEXT("pure"), PureParam);

		TSharedPtr<FJsonObject> ConstParam = MakeShared<FJsonObject>();
		ConstParam->SetStringField(TEXT("type"), TEXT("boolean"));
		ConstParam->SetStringField(TEXT("description"), TEXT("Optional const flag."));
		Tool.Parameters->SetObjectField(TEXT("const"), ConstParam);

		TSharedPtr<FJsonObject> CallInEditorParam = MakeShared<FJsonObject>();
		CallInEditorParam->SetStringField(TEXT("type"), TEXT("boolean"));
		CallInEditorParam->SetStringField(TEXT("description"), TEXT("Optional call-in-editor flag."));
		Tool.Parameters->SetObjectField(TEXT("call_in_editor"), CallInEditorParam);

		TSharedPtr<FJsonObject> AccessParam = MakeShared<FJsonObject>();
		AccessParam->SetStringField(TEXT("type"), TEXT("string"));
		AccessParam->SetStringField(TEXT("description"), TEXT("Optional access specifier: public, protected, private."));
		Tool.Parameters->SetObjectField(TEXT("access"), AccessParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("function_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("add_function_param");
		Tool.Description = TEXT("Add a function input or output parameter pin.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> FunctionParam = MakeShared<FJsonObject>();
		FunctionParam->SetStringField(TEXT("type"), TEXT("string"));
		FunctionParam->SetStringField(TEXT("description"), TEXT("Function name."));
		Tool.Parameters->SetObjectField(TEXT("function_name"), FunctionParam);

		TSharedPtr<FJsonObject> ParamNameParam = MakeShared<FJsonObject>();
		ParamNameParam->SetStringField(TEXT("type"), TEXT("string"));
		ParamNameParam->SetStringField(TEXT("description"), TEXT("Parameter name."));
		Tool.Parameters->SetObjectField(TEXT("param_name"), ParamNameParam);

		TSharedPtr<FJsonObject> ParamTypeParam = MakeShared<FJsonObject>();
		ParamTypeParam->SetStringField(TEXT("type"), TEXT("string"));
		ParamTypeParam->SetStringField(TEXT("description"), TEXT("Parameter type (supports array<T>, set<T>, map<K,V>)."));
		Tool.Parameters->SetObjectField(TEXT("param_type"), ParamTypeParam);

		TSharedPtr<FJsonObject> DirectionParam = MakeShared<FJsonObject>();
		DirectionParam->SetStringField(TEXT("type"), TEXT("string"));
		DirectionParam->SetStringField(TEXT("description"), TEXT("Optional direction: input (default) or output."));
		Tool.Parameters->SetObjectField(TEXT("direction"), DirectionParam);

		TSharedPtr<FJsonObject> TypeRefParam = MakeShared<FJsonObject>();
		TypeRefParam->SetStringField(TEXT("type"), TEXT("string"));
		TypeRefParam->SetStringField(TEXT("description"), TEXT("Optional type reference path for object/class/enum/struct/interface types."));
		Tool.Parameters->SetObjectField(TEXT("type_reference"), TypeRefParam);

		TSharedPtr<FJsonObject> ContainerParam = MakeShared<FJsonObject>();
		ContainerParam->SetStringField(TEXT("type"), TEXT("string"));
		ContainerParam->SetStringField(TEXT("description"), TEXT("Optional container override: none, array, set, map."));
		Tool.Parameters->SetObjectField(TEXT("container_type"), ContainerParam);

		TSharedPtr<FJsonObject> MapKeyTypeParam = MakeShared<FJsonObject>();
		MapKeyTypeParam->SetStringField(TEXT("type"), TEXT("string"));
		MapKeyTypeParam->SetStringField(TEXT("description"), TEXT("Optional map key type when using map container."));
		Tool.Parameters->SetObjectField(TEXT("map_key_type"), MapKeyTypeParam);

		TSharedPtr<FJsonObject> MapKeyRefParam = MakeShared<FJsonObject>();
		MapKeyRefParam->SetStringField(TEXT("type"), TEXT("string"));
		MapKeyRefParam->SetStringField(TEXT("description"), TEXT("Optional referenced type path for map key type."));
		Tool.Parameters->SetObjectField(TEXT("map_key_type_reference"), MapKeyRefParam);

		TSharedPtr<FJsonObject> RefParam = MakeShared<FJsonObject>();
		RefParam->SetStringField(TEXT("type"), TEXT("boolean"));
		RefParam->SetStringField(TEXT("description"), TEXT("Optional pass-by-reference flag (input params only)."));
		Tool.Parameters->SetObjectField(TEXT("pass_by_reference"), RefParam);

		TSharedPtr<FJsonObject> ConstParam = MakeShared<FJsonObject>();
		ConstParam->SetStringField(TEXT("type"), TEXT("boolean"));
		ConstParam->SetStringField(TEXT("description"), TEXT("Optional const flag."));
		Tool.Parameters->SetObjectField(TEXT("const"), ConstParam);

		TSharedPtr<FJsonObject> DefaultParam = MakeShared<FJsonObject>();
		DefaultParam->SetStringField(TEXT("type"), TEXT("string"));
		DefaultParam->SetStringField(TEXT("description"), TEXT("Optional default value for input params."));
		Tool.Parameters->SetObjectField(TEXT("default_value"), DefaultParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("function_name"));
		Tool.RequiredParams.Add(TEXT("param_name"));
		Tool.RequiredParams.Add(TEXT("param_type"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("remove_function_param");
		Tool.Description = TEXT("Remove a function input or output parameter pin by name.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> FunctionParam = MakeShared<FJsonObject>();
		FunctionParam->SetStringField(TEXT("type"), TEXT("string"));
		FunctionParam->SetStringField(TEXT("description"), TEXT("Function name."));
		Tool.Parameters->SetObjectField(TEXT("function_name"), FunctionParam);

		TSharedPtr<FJsonObject> ParamNameParam = MakeShared<FJsonObject>();
		ParamNameParam->SetStringField(TEXT("type"), TEXT("string"));
		ParamNameParam->SetStringField(TEXT("description"), TEXT("Parameter name to remove."));
		Tool.Parameters->SetObjectField(TEXT("param_name"), ParamNameParam);

		TSharedPtr<FJsonObject> DirectionParam = MakeShared<FJsonObject>();
		DirectionParam->SetStringField(TEXT("type"), TEXT("string"));
		DirectionParam->SetStringField(TEXT("description"), TEXT("Optional direction filter: input or output."));
		Tool.Parameters->SetObjectField(TEXT("direction"), DirectionParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("function_name"));
		Tool.RequiredParams.Add(TEXT("param_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("set_function_return");
		Tool.Description = TEXT("Create/update/remove a function return pin.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> FunctionParam = MakeShared<FJsonObject>();
		FunctionParam->SetStringField(TEXT("type"), TEXT("string"));
		FunctionParam->SetStringField(TEXT("description"), TEXT("Function name."));
		Tool.Parameters->SetObjectField(TEXT("function_name"), FunctionParam);

		TSharedPtr<FJsonObject> ClearParam = MakeShared<FJsonObject>();
		ClearParam->SetStringField(TEXT("type"), TEXT("boolean"));
		ClearParam->SetStringField(TEXT("description"), TEXT("True to remove the return pin by return_name (default ReturnValue)."));
		Tool.Parameters->SetObjectField(TEXT("clear"), ClearParam);

		TSharedPtr<FJsonObject> ReturnNameParam = MakeShared<FJsonObject>();
		ReturnNameParam->SetStringField(TEXT("type"), TEXT("string"));
		ReturnNameParam->SetStringField(TEXT("description"), TEXT("Optional return pin name (default: ReturnValue)."));
		Tool.Parameters->SetObjectField(TEXT("return_name"), ReturnNameParam);

		TSharedPtr<FJsonObject> ReturnTypeParam = MakeShared<FJsonObject>();
		ReturnTypeParam->SetStringField(TEXT("type"), TEXT("string"));
		ReturnTypeParam->SetStringField(TEXT("description"), TEXT("Return type when clear=false (supports array<T>, set<T>, map<K,V>)."));
		Tool.Parameters->SetObjectField(TEXT("return_type"), ReturnTypeParam);

		TSharedPtr<FJsonObject> TypeRefParam = MakeShared<FJsonObject>();
		TypeRefParam->SetStringField(TEXT("type"), TEXT("string"));
		TypeRefParam->SetStringField(TEXT("description"), TEXT("Optional return type reference path."));
		Tool.Parameters->SetObjectField(TEXT("type_reference"), TypeRefParam);

		TSharedPtr<FJsonObject> ContainerParam = MakeShared<FJsonObject>();
		ContainerParam->SetStringField(TEXT("type"), TEXT("string"));
		ContainerParam->SetStringField(TEXT("description"), TEXT("Optional container override: none, array, set, map."));
		Tool.Parameters->SetObjectField(TEXT("container_type"), ContainerParam);

		TSharedPtr<FJsonObject> MapKeyTypeParam = MakeShared<FJsonObject>();
		MapKeyTypeParam->SetStringField(TEXT("type"), TEXT("string"));
		MapKeyTypeParam->SetStringField(TEXT("description"), TEXT("Optional map key type when using map return type."));
		Tool.Parameters->SetObjectField(TEXT("map_key_type"), MapKeyTypeParam);

		TSharedPtr<FJsonObject> MapKeyRefParam = MakeShared<FJsonObject>();
		MapKeyRefParam->SetStringField(TEXT("type"), TEXT("string"));
		MapKeyRefParam->SetStringField(TEXT("description"), TEXT("Optional referenced type path for map key type."));
		Tool.Parameters->SetObjectField(TEXT("map_key_type_reference"), MapKeyRefParam);

		TSharedPtr<FJsonObject> DefaultParam = MakeShared<FJsonObject>();
		DefaultParam->SetStringField(TEXT("type"), TEXT("string"));
		DefaultParam->SetStringField(TEXT("description"), TEXT("Optional default value for the return pin."));
		Tool.Parameters->SetObjectField(TEXT("default_value"), DefaultParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("function_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("list_macros");
		Tool.Description = TEXT("List top-level Blueprint macro graphs.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("create_macro");
		Tool.Description = TEXT("Create a new Blueprint macro graph.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> NameParam = MakeShared<FJsonObject>();
		NameParam->SetStringField(TEXT("type"), TEXT("string"));
		NameParam->SetStringField(TEXT("description"), TEXT("Macro name."));
		Tool.Parameters->SetObjectField(TEXT("macro_name"), NameParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("macro_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("delete_macro");
		Tool.Description = TEXT("Delete a Blueprint macro graph.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> NameParam = MakeShared<FJsonObject>();
		NameParam->SetStringField(TEXT("type"), TEXT("string"));
		NameParam->SetStringField(TEXT("description"), TEXT("Macro name."));
		Tool.Parameters->SetObjectField(TEXT("macro_name"), NameParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("macro_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("list_event_dispatchers");
		Tool.Description = TEXT("List Blueprint event dispatchers and their signature parameters.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("create_event_dispatcher");
		Tool.Description = TEXT("Create a new Blueprint event dispatcher.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> NameParam = MakeShared<FJsonObject>();
		NameParam->SetStringField(TEXT("type"), TEXT("string"));
		NameParam->SetStringField(TEXT("description"), TEXT("Dispatcher name."));
		Tool.Parameters->SetObjectField(TEXT("dispatcher_name"), NameParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("dispatcher_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("set_dispatcher_signature");
		Tool.Description = TEXT("Replace dispatcher signature parameters with a provided parameter list.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> NameParam = MakeShared<FJsonObject>();
		NameParam->SetStringField(TEXT("type"), TEXT("string"));
		NameParam->SetStringField(TEXT("description"), TEXT("Dispatcher name."));
		Tool.Parameters->SetObjectField(TEXT("dispatcher_name"), NameParam);

		TSharedPtr<FJsonObject> ParamsParam = MakeShared<FJsonObject>();
		ParamsParam->SetStringField(TEXT("type"), TEXT("array"));
		ParamsParam->SetStringField(TEXT("description"), TEXT("Array of signature parameters. Each item supports: param_name, param_type, type_reference, container_type, map_key_type, map_key_type_reference, default_value."));
		Tool.Parameters->SetObjectField(TEXT("parameters"), ParamsParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("dispatcher_name"));
		Tool.RequiredParams.Add(TEXT("parameters"));
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
		Tool.Name = TEXT("add_node_by_class");
		Tool.Description = TEXT("Spawn a graph node from a node class path or class name.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> ClassParam = MakeShared<FJsonObject>();
		ClassParam->SetStringField(TEXT("type"), TEXT("string"));
		ClassParam->SetStringField(TEXT("description"), TEXT("Node class path or class name (must derive from UEdGraphNode)."));
		Tool.Parameters->SetObjectField(TEXT("node_class"), ClassParam);

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
		Tool.RequiredParams.Add(TEXT("node_class"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("add_custom_event_node");
		Tool.Description = TEXT("Add a Custom Event node to a Blueprint graph.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> EventParam = MakeShared<FJsonObject>();
		EventParam->SetStringField(TEXT("type"), TEXT("string"));
		EventParam->SetStringField(TEXT("description"), TEXT("Custom event name."));
		Tool.Parameters->SetObjectField(TEXT("event_name"), EventParam);

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

		TSharedPtr<FJsonObject> CallInEditorParam = MakeShared<FJsonObject>();
		CallInEditorParam->SetStringField(TEXT("type"), TEXT("boolean"));
		CallInEditorParam->SetStringField(TEXT("description"), TEXT("Whether the custom event can be called in editor (default: false)."));
		Tool.Parameters->SetObjectField(TEXT("call_in_editor"), CallInEditorParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("event_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("add_comment_node");
		Tool.Description = TEXT("Add a comment box node to a Blueprint graph.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> GraphParam = MakeShared<FJsonObject>();
		GraphParam->SetStringField(TEXT("type"), TEXT("string"));
		GraphParam->SetStringField(TEXT("description"), TEXT("Graph name (default: EventGraph)."));
		Tool.Parameters->SetObjectField(TEXT("graph_name"), GraphParam);

		TSharedPtr<FJsonObject> CommentParam = MakeShared<FJsonObject>();
		CommentParam->SetStringField(TEXT("type"), TEXT("string"));
		CommentParam->SetStringField(TEXT("description"), TEXT("Comment text (default: Comment)."));
		Tool.Parameters->SetObjectField(TEXT("comment"), CommentParam);

		TSharedPtr<FJsonObject> XParam = MakeShared<FJsonObject>();
		XParam->SetStringField(TEXT("type"), TEXT("number"));
		XParam->SetStringField(TEXT("description"), TEXT("Node X position (default: 0)."));
		Tool.Parameters->SetObjectField(TEXT("x"), XParam);

		TSharedPtr<FJsonObject> YParam = MakeShared<FJsonObject>();
		YParam->SetStringField(TEXT("type"), TEXT("number"));
		YParam->SetStringField(TEXT("description"), TEXT("Node Y position (default: 0)."));
		Tool.Parameters->SetObjectField(TEXT("y"), YParam);

		TSharedPtr<FJsonObject> WidthParam = MakeShared<FJsonObject>();
		WidthParam->SetStringField(TEXT("type"), TEXT("number"));
		WidthParam->SetStringField(TEXT("description"), TEXT("Comment box width (default: 400)."));
		Tool.Parameters->SetObjectField(TEXT("width"), WidthParam);

		TSharedPtr<FJsonObject> HeightParam = MakeShared<FJsonObject>();
		HeightParam->SetStringField(TEXT("type"), TEXT("number"));
		HeightParam->SetStringField(TEXT("description"), TEXT("Comment box height (default: 200)."));
		Tool.Parameters->SetObjectField(TEXT("height"), HeightParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("add_reroute_node");
		Tool.Description = TEXT("Add a reroute (knot) node to a Blueprint graph.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

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
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("delete_node");
		Tool.Description = TEXT("Delete a node from a Blueprint graph.");

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
		NodeParam->SetStringField(TEXT("description"), TEXT("Node id to delete."));
		Tool.Parameters->SetObjectField(TEXT("node_id"), NodeParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("node_id"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("duplicate_node");
		Tool.Description = TEXT("Duplicate one or more nodes in a Blueprint graph.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> GraphParam = MakeShared<FJsonObject>();
		GraphParam->SetStringField(TEXT("type"), TEXT("string"));
		GraphParam->SetStringField(TEXT("description"), TEXT("Graph name (default: EventGraph)."));
		Tool.Parameters->SetObjectField(TEXT("graph_name"), GraphParam);

		TSharedPtr<FJsonObject> NodeIdsParam = MakeShared<FJsonObject>();
		NodeIdsParam->SetStringField(TEXT("type"), TEXT("array"));
		NodeIdsParam->SetStringField(TEXT("description"), TEXT("Array of node ids to duplicate."));
		Tool.Parameters->SetObjectField(TEXT("node_ids"), NodeIdsParam);

		TSharedPtr<FJsonObject> OffsetXParam = MakeShared<FJsonObject>();
		OffsetXParam->SetStringField(TEXT("type"), TEXT("number"));
		OffsetXParam->SetStringField(TEXT("description"), TEXT("Duplicate offset X (default: 40)."));
		Tool.Parameters->SetObjectField(TEXT("offset_x"), OffsetXParam);

		TSharedPtr<FJsonObject> OffsetYParam = MakeShared<FJsonObject>();
		OffsetYParam->SetStringField(TEXT("type"), TEXT("number"));
		OffsetYParam->SetStringField(TEXT("description"), TEXT("Duplicate offset Y (default: 40)."));
		Tool.Parameters->SetObjectField(TEXT("offset_y"), OffsetYParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("node_ids"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("move_node");
		Tool.Description = TEXT("Move a node to absolute position or by delta.");

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
		NodeParam->SetStringField(TEXT("description"), TEXT("Node id to move."));
		Tool.Parameters->SetObjectField(TEXT("node_id"), NodeParam);

		TSharedPtr<FJsonObject> XParam = MakeShared<FJsonObject>();
		XParam->SetStringField(TEXT("type"), TEXT("number"));
		XParam->SetStringField(TEXT("description"), TEXT("Absolute X position."));
		Tool.Parameters->SetObjectField(TEXT("x"), XParam);

		TSharedPtr<FJsonObject> YParam = MakeShared<FJsonObject>();
		YParam->SetStringField(TEXT("type"), TEXT("number"));
		YParam->SetStringField(TEXT("description"), TEXT("Absolute Y position."));
		Tool.Parameters->SetObjectField(TEXT("y"), YParam);

		TSharedPtr<FJsonObject> DeltaXParam = MakeShared<FJsonObject>();
		DeltaXParam->SetStringField(TEXT("type"), TEXT("number"));
		DeltaXParam->SetStringField(TEXT("description"), TEXT("Relative X delta."));
		Tool.Parameters->SetObjectField(TEXT("delta_x"), DeltaXParam);

		TSharedPtr<FJsonObject> DeltaYParam = MakeShared<FJsonObject>();
		DeltaYParam->SetStringField(TEXT("type"), TEXT("number"));
		DeltaYParam->SetStringField(TEXT("description"), TEXT("Relative Y delta."));
		Tool.Parameters->SetObjectField(TEXT("delta_y"), DeltaYParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("node_id"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("rename_node");
		Tool.Description = TEXT("Rename a node when the node type supports rename.");

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
		NodeParam->SetStringField(TEXT("description"), TEXT("Node id to rename."));
		Tool.Parameters->SetObjectField(TEXT("node_id"), NodeParam);

		TSharedPtr<FJsonObject> NameParam = MakeShared<FJsonObject>();
		NameParam->SetStringField(TEXT("type"), TEXT("string"));
		NameParam->SetStringField(TEXT("description"), TEXT("New node name."));
		Tool.Parameters->SetObjectField(TEXT("new_name"), NameParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("node_id"));
		Tool.RequiredParams.Add(TEXT("new_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("set_node_comment");
		Tool.Description = TEXT("Set the node comment text and optional bubble visibility.");

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
		NodeParam->SetStringField(TEXT("description"), TEXT("Node id to update."));
		Tool.Parameters->SetObjectField(TEXT("node_id"), NodeParam);

		TSharedPtr<FJsonObject> CommentParam = MakeShared<FJsonObject>();
		CommentParam->SetStringField(TEXT("type"), TEXT("string"));
		CommentParam->SetStringField(TEXT("description"), TEXT("Node comment text."));
		Tool.Parameters->SetObjectField(TEXT("comment"), CommentParam);

		TSharedPtr<FJsonObject> BubbleParam = MakeShared<FJsonObject>();
		BubbleParam->SetStringField(TEXT("type"), TEXT("boolean"));
		BubbleParam->SetStringField(TEXT("description"), TEXT("Comment bubble visibility override."));
		Tool.Parameters->SetObjectField(TEXT("bubble_visible"), BubbleParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("node_id"));
		Tool.RequiredParams.Add(TEXT("comment"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("collapse_nodes_to_function");
		Tool.Description = TEXT("Collapse selected nodes into a new function and insert a function call node.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> GraphParam = MakeShared<FJsonObject>();
		GraphParam->SetStringField(TEXT("type"), TEXT("string"));
		GraphParam->SetStringField(TEXT("description"), TEXT("Source graph name (default: EventGraph)."));
		Tool.Parameters->SetObjectField(TEXT("graph_name"), GraphParam);

		TSharedPtr<FJsonObject> NodeIdsParam = MakeShared<FJsonObject>();
		NodeIdsParam->SetStringField(TEXT("type"), TEXT("array"));
		NodeIdsParam->SetStringField(TEXT("description"), TEXT("Array of node ids to collapse."));
		Tool.Parameters->SetObjectField(TEXT("node_ids"), NodeIdsParam);

		TSharedPtr<FJsonObject> FunctionNameParam = MakeShared<FJsonObject>();
		FunctionNameParam->SetStringField(TEXT("type"), TEXT("string"));
		FunctionNameParam->SetStringField(TEXT("description"), TEXT("Optional target function name."));
		Tool.Parameters->SetObjectField(TEXT("function_name"), FunctionNameParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("node_ids"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("collapse_nodes_to_macro");
		Tool.Description = TEXT("Collapse selected nodes into a new macro and insert a macro instance node.");

		TSharedPtr<FJsonObject> PathParam = MakeShared<FJsonObject>();
		PathParam->SetStringField(TEXT("type"), TEXT("string"));
		PathParam->SetStringField(TEXT("description"), TEXT("Blueprint asset path."));
		Tool.Parameters->SetObjectField(TEXT("blueprint_path"), PathParam);

		TSharedPtr<FJsonObject> GraphParam = MakeShared<FJsonObject>();
		GraphParam->SetStringField(TEXT("type"), TEXT("string"));
		GraphParam->SetStringField(TEXT("description"), TEXT("Source graph name (default: EventGraph)."));
		Tool.Parameters->SetObjectField(TEXT("graph_name"), GraphParam);

		TSharedPtr<FJsonObject> NodeIdsParam = MakeShared<FJsonObject>();
		NodeIdsParam->SetStringField(TEXT("type"), TEXT("array"));
		NodeIdsParam->SetStringField(TEXT("description"), TEXT("Array of node ids to collapse."));
		Tool.Parameters->SetObjectField(TEXT("node_ids"), NodeIdsParam);

		TSharedPtr<FJsonObject> MacroNameParam = MakeShared<FJsonObject>();
		MacroNameParam->SetStringField(TEXT("type"), TEXT("string"));
		MacroNameParam->SetStringField(TEXT("description"), TEXT("Optional target macro name."));
		Tool.Parameters->SetObjectField(TEXT("macro_name"), MacroNameParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("node_ids"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("list_node_pins");
		Tool.Description = TEXT("List pins on a node, including split pins and linked connections.");

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
		NodeParam->SetStringField(TEXT("description"), TEXT("Node id."));
		Tool.Parameters->SetObjectField(TEXT("node_id"), NodeParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("node_id"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("disconnect_pins");
		Tool.Description = TEXT("Disconnect one specific link between two pins.");

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
		FromPinParam->SetStringField(TEXT("description"), TEXT("Source pin name or split pin path."));
		Tool.Parameters->SetObjectField(TEXT("from_pin"), FromPinParam);

		TSharedPtr<FJsonObject> ToNodeParam = MakeShared<FJsonObject>();
		ToNodeParam->SetStringField(TEXT("type"), TEXT("string"));
		ToNodeParam->SetStringField(TEXT("description"), TEXT("Target node_id."));
		Tool.Parameters->SetObjectField(TEXT("to_node_id"), ToNodeParam);

		TSharedPtr<FJsonObject> ToPinParam = MakeShared<FJsonObject>();
		ToPinParam->SetStringField(TEXT("type"), TEXT("string"));
		ToPinParam->SetStringField(TEXT("description"), TEXT("Target pin name or split pin path."));
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
		Tool.Name = TEXT("break_pin_links");
		Tool.Description = TEXT("Break all connections on a pin.");

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
		NodeParam->SetStringField(TEXT("description"), TEXT("Node id."));
		Tool.Parameters->SetObjectField(TEXT("node_id"), NodeParam);

		TSharedPtr<FJsonObject> PinParam = MakeShared<FJsonObject>();
		PinParam->SetStringField(TEXT("type"), TEXT("string"));
		PinParam->SetStringField(TEXT("description"), TEXT("Pin name or split pin path."));
		Tool.Parameters->SetObjectField(TEXT("pin_name"), PinParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("node_id"));
		Tool.RequiredParams.Add(TEXT("pin_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("break_all_node_links");
		Tool.Description = TEXT("Break all links on every pin of a node.");

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
		NodeParam->SetStringField(TEXT("description"), TEXT("Node id."));
		Tool.Parameters->SetObjectField(TEXT("node_id"), NodeParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("node_id"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("reset_pin_default_value");
		Tool.Description = TEXT("Reset a pin default value to autogenerated default.");

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
		NodeParam->SetStringField(TEXT("description"), TEXT("Node id."));
		Tool.Parameters->SetObjectField(TEXT("node_id"), NodeParam);

		TSharedPtr<FJsonObject> PinParam = MakeShared<FJsonObject>();
		PinParam->SetStringField(TEXT("type"), TEXT("string"));
		PinParam->SetStringField(TEXT("description"), TEXT("Pin name or split pin path."));
		Tool.Parameters->SetObjectField(TEXT("pin_name"), PinParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("node_id"));
		Tool.RequiredParams.Add(TEXT("pin_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("split_struct_pin");
		Tool.Description = TEXT("Split a struct pin into member sub-pins.");

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
		NodeParam->SetStringField(TEXT("description"), TEXT("Node id."));
		Tool.Parameters->SetObjectField(TEXT("node_id"), NodeParam);

		TSharedPtr<FJsonObject> PinParam = MakeShared<FJsonObject>();
		PinParam->SetStringField(TEXT("type"), TEXT("string"));
		PinParam->SetStringField(TEXT("description"), TEXT("Parent struct pin name or path."));
		Tool.Parameters->SetObjectField(TEXT("pin_name"), PinParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("node_id"));
		Tool.RequiredParams.Add(TEXT("pin_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("recombine_struct_pin");
		Tool.Description = TEXT("Recombine a split struct pin back to a single pin.");

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
		NodeParam->SetStringField(TEXT("description"), TEXT("Node id."));
		Tool.Parameters->SetObjectField(TEXT("node_id"), NodeParam);

		TSharedPtr<FJsonObject> PinParam = MakeShared<FJsonObject>();
		PinParam->SetStringField(TEXT("type"), TEXT("string"));
		PinParam->SetStringField(TEXT("description"), TEXT("A child split pin path or parent struct pin path."));
		Tool.Parameters->SetObjectField(TEXT("pin_name"), PinParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("node_id"));
		Tool.RequiredParams.Add(TEXT("pin_name"));
		Tools.Add(Tool);
	}

	{
		FMCPToolInfo Tool;
		Tool.Name = TEXT("promote_pin_to_variable");
		Tool.Description = TEXT("Promote a pin to a new variable and spawn a variable get/set node.");

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
		NodeParam->SetStringField(TEXT("description"), TEXT("Node id."));
		Tool.Parameters->SetObjectField(TEXT("node_id"), NodeParam);

		TSharedPtr<FJsonObject> PinParam = MakeShared<FJsonObject>();
		PinParam->SetStringField(TEXT("type"), TEXT("string"));
		PinParam->SetStringField(TEXT("description"), TEXT("Pin name or split pin path."));
		Tool.Parameters->SetObjectField(TEXT("pin_name"), PinParam);

		TSharedPtr<FJsonObject> NameParam = MakeShared<FJsonObject>();
		NameParam->SetStringField(TEXT("type"), TEXT("string"));
		NameParam->SetStringField(TEXT("description"), TEXT("Optional requested variable name."));
		Tool.Parameters->SetObjectField(TEXT("variable_name"), NameParam);

		TSharedPtr<FJsonObject> ScopeParam = MakeShared<FJsonObject>();
		ScopeParam->SetStringField(TEXT("type"), TEXT("boolean"));
		ScopeParam->SetStringField(TEXT("description"), TEXT("true: member variable (default), false: local variable when supported."));
		Tool.Parameters->SetObjectField(TEXT("to_member_variable"), ScopeParam);

		TSharedPtr<FJsonObject> XParam = MakeShared<FJsonObject>();
		XParam->SetStringField(TEXT("type"), TEXT("number"));
		XParam->SetStringField(TEXT("description"), TEXT("Optional variable node X position."));
		Tool.Parameters->SetObjectField(TEXT("x"), XParam);

		TSharedPtr<FJsonObject> YParam = MakeShared<FJsonObject>();
		YParam->SetStringField(TEXT("type"), TEXT("number"));
		YParam->SetStringField(TEXT("description"), TEXT("Optional variable node Y position."));
		Tool.Parameters->SetObjectField(TEXT("y"), YParam);

		Tool.RequiredParams.Add(TEXT("blueprint_path"));
		Tool.RequiredParams.Add(TEXT("node_id"));
		Tool.RequiredParams.Add(TEXT("pin_name"));
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
	if (MethodName == TEXT("list_components")) return HandleListComponents(Request);
	if (MethodName == TEXT("add_component")) return HandleAddComponent(Request);
	if (MethodName == TEXT("remove_component")) return HandleRemoveComponent(Request);
	if (MethodName == TEXT("rename_component")) return HandleRenameComponent(Request);
	if (MethodName == TEXT("set_root_component")) return HandleSetRootComponent(Request);
	if (MethodName == TEXT("attach_component")) return HandleAttachComponent(Request);
	if (MethodName == TEXT("detach_component")) return HandleDetachComponent(Request);
	if (MethodName == TEXT("set_component_property")) return HandleSetComponentProperty(Request);
	if (MethodName == TEXT("get_component_property")) return HandleGetComponentProperty(Request);
	if (MethodName == TEXT("set_component_transform_default")) return HandleSetComponentTransformDefault(Request);
	if (MethodName == TEXT("list_functions")) return HandleListFunctions(Request);
	if (MethodName == TEXT("create_function")) return HandleCreateFunction(Request);
	if (MethodName == TEXT("delete_function")) return HandleDeleteFunction(Request);
	if (MethodName == TEXT("rename_function")) return HandleRenameFunction(Request);
	if (MethodName == TEXT("set_function_flags")) return HandleSetFunctionFlags(Request);
	if (MethodName == TEXT("add_function_param")) return HandleAddFunctionParam(Request);
	if (MethodName == TEXT("remove_function_param")) return HandleRemoveFunctionParam(Request);
	if (MethodName == TEXT("set_function_return")) return HandleSetFunctionReturn(Request);
	if (MethodName == TEXT("list_macros")) return HandleListMacros(Request);
	if (MethodName == TEXT("create_macro")) return HandleCreateMacro(Request);
	if (MethodName == TEXT("delete_macro")) return HandleDeleteMacro(Request);
	if (MethodName == TEXT("list_event_dispatchers")) return HandleListEventDispatchers(Request);
	if (MethodName == TEXT("create_event_dispatcher")) return HandleCreateEventDispatcher(Request);
	if (MethodName == TEXT("set_dispatcher_signature")) return HandleSetDispatcherSignature(Request);
	if (MethodName == TEXT("add_event_node")) return HandleAddEventNode(Request);
	if (MethodName == TEXT("add_call_function_node")) return HandleAddCallFunctionNode(Request);
	if (MethodName == TEXT("add_variable_get_node")) return HandleAddVariableGetNode(Request);
	if (MethodName == TEXT("add_variable_set_node")) return HandleAddVariableSetNode(Request);
	if (MethodName == TEXT("add_node_by_class")) return HandleAddNodeByClass(Request);
	if (MethodName == TEXT("add_custom_event_node")) return HandleAddCustomEventNode(Request);
	if (MethodName == TEXT("add_comment_node")) return HandleAddCommentNode(Request);
	if (MethodName == TEXT("add_reroute_node")) return HandleAddRerouteNode(Request);
	if (MethodName == TEXT("delete_node")) return HandleDeleteNode(Request);
	if (MethodName == TEXT("duplicate_node")) return HandleDuplicateNode(Request);
	if (MethodName == TEXT("move_node")) return HandleMoveNode(Request);
	if (MethodName == TEXT("rename_node")) return HandleRenameNode(Request);
	if (MethodName == TEXT("set_node_comment")) return HandleSetNodeComment(Request);
	if (MethodName == TEXT("collapse_nodes_to_function")) return HandleCollapseNodesToFunction(Request);
	if (MethodName == TEXT("collapse_nodes_to_macro")) return HandleCollapseNodesToMacro(Request);
	if (MethodName == TEXT("list_node_pins")) return HandleListNodePins(Request);
	if (MethodName == TEXT("disconnect_pins")) return HandleDisconnectPins(Request);
	if (MethodName == TEXT("break_pin_links")) return HandleBreakPinLinks(Request);
	if (MethodName == TEXT("break_all_node_links")) return HandleBreakAllNodeLinks(Request);
	if (MethodName == TEXT("reset_pin_default_value")) return HandleResetPinDefaultValue(Request);
	if (MethodName == TEXT("split_struct_pin")) return HandleSplitStructPin(Request);
	if (MethodName == TEXT("recombine_struct_pin")) return HandleRecombineStructPin(Request);
	if (MethodName == TEXT("promote_pin_to_variable")) return HandlePromotePinToVariable(Request);
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
	return FindPinByPathOrName(Node, PinName);
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

FMCPResponse FBlueprintService::HandleListComponents(const FMCPRequest& Request)
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

		USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
		if (!SCS)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Blueprint does not have a SimpleConstructionScript"));
			return Result;
		}

		TArray<TSharedPtr<FJsonValue>> ComponentsJson;
		for (USCS_Node* Node : SCS->GetAllNodes())
		{
			if (!Node)
			{
				continue;
			}
			ComponentsJson.Add(MakeShared<FJsonValueObject>(BuildComponentJson(Blueprint, SCS, Node)));
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetArrayField(TEXT("components"), ComponentsJson);
		Result->SetNumberField(TEXT("count"), ComponentsJson.Num());
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleAddComponent(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString ComponentClassNameOrPath;
	FString ComponentName;
	FString ParentComponentName;
	FString SocketName;

	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("component_class"), ComponentClassNameOrPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'component_class'"));
	}
	Request.Params->TryGetStringField(TEXT("component_name"), ComponentName);
	Request.Params->TryGetStringField(TEXT("parent_component_name"), ParentComponentName);
	Request.Params->TryGetStringField(TEXT("socket_name"), SocketName);

	auto Task = [BlueprintPath, ComponentClassNameOrPath, ComponentName, ParentComponentName, SocketName]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return Result;
		}

		USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
		if (!SCS)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Blueprint does not have a SimpleConstructionScript"));
			return Result;
		}

		UClass* ComponentClass = ResolveClassByNameOrPath(ComponentClassNameOrPath);
		if (!ComponentClass)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Component class not found: %s"), *ComponentClassNameOrPath));
			return Result;
		}
		if (!ComponentClass->IsChildOf(UActorComponent::StaticClass()))
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Class is not an ActorComponent: %s"), *ComponentClass->GetPathName()));
			return Result;
		}

		const FName NewComponentName = ComponentName.IsEmpty() ? NAME_None : FName(*ComponentName);
		if (!NewComponentName.IsNone() && SCS->FindSCSNode(NewComponentName))
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Component already exists: %s"), *ComponentName));
			return Result;
		}

		USCS_Node* NewNode = SCS->CreateNode(ComponentClass, NewComponentName);
		if (!NewNode)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Failed to create component node"));
			return Result;
		}

		if (!ParentComponentName.IsEmpty())
		{
			USCS_Node* ParentNode = SCS->FindSCSNode(FName(*ParentComponentName));
			if (!ParentNode)
			{
				Result->SetBoolField(TEXT("success"), false);
				Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Parent component not found: %s"), *ParentComponentName));
				return Result;
			}

			if (!Cast<USceneComponent>(ParentNode->ComponentTemplate))
			{
				Result->SetBoolField(TEXT("success"), false);
				Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Parent component is not a scene component: %s"), *ParentComponentName));
				return Result;
			}
			if (!Cast<USceneComponent>(NewNode->ComponentTemplate))
			{
				Result->SetBoolField(TEXT("success"), false);
				Result->SetStringField(TEXT("error"), TEXT("Only scene components can be attached to a parent component"));
				return Result;
			}

			ParentNode->AddChildNode(NewNode, true);
			NewNode->SetParent(ParentNode);
		}
		else
		{
			SCS->AddNode(NewNode);
		}

		if (!SocketName.IsEmpty())
		{
			NewNode->Modify();
			NewNode->AttachToName = FName(*SocketName);
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetObjectField(TEXT("component"), BuildComponentJson(Blueprint, SCS, NewNode));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleRemoveComponent(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString ComponentName;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("component_name"), ComponentName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'component_name'"));
	}

	auto Task = [BlueprintPath, ComponentName]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return Result;
		}

		USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
		if (!SCS)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Blueprint does not have a SimpleConstructionScript"));
			return Result;
		}

		USCS_Node* TargetNode = SCS->FindSCSNode(FName(*ComponentName));
		if (!TargetNode)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Component not found: %s"), *ComponentName));
			return Result;
		}

		SCS->RemoveNodeAndPromoteChildren(TargetNode);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetStringField(TEXT("removed_component"), ComponentName);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleRenameComponent(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString ComponentName;
	FString NewComponentName;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("component_name"), ComponentName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'component_name'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("new_component_name"), NewComponentName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'new_component_name'"));
	}

	auto Task = [BlueprintPath, ComponentName, NewComponentName]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return Result;
		}

		USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
		if (!SCS)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Blueprint does not have a SimpleConstructionScript"));
			return Result;
		}

		USCS_Node* Node = SCS->FindSCSNode(FName(*ComponentName));
		if (!Node)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Component not found: %s"), *ComponentName));
			return Result;
		}

		const FName NewName(*NewComponentName);
		if (Node->GetVariableName() != NewName && SCS->FindSCSNode(NewName))
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("A component with that name already exists: %s"), *NewComponentName));
			return Result;
		}

		FBlueprintEditorUtils::RenameComponentMemberVariable(Blueprint, Node, NewName);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		USCS_Node* RenamedNode = SCS->FindSCSNode(NewName);
		if (!RenamedNode)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Component rename failed"));
			return Result;
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetStringField(TEXT("old_component_name"), ComponentName);
		Result->SetStringField(TEXT("new_component_name"), NewComponentName);
		Result->SetObjectField(TEXT("component"), BuildComponentJson(Blueprint, SCS, RenamedNode));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleSetRootComponent(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString ComponentName;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("component_name"), ComponentName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'component_name'"));
	}

	auto Task = [BlueprintPath, ComponentName]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return Result;
		}

		USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
		if (!SCS)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Blueprint does not have a SimpleConstructionScript"));
			return Result;
		}

		USCS_Node* TargetNode = SCS->FindSCSNode(FName(*ComponentName));
		if (!TargetNode)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Component not found: %s"), *ComponentName));
			return Result;
		}
		if (!Cast<USceneComponent>(TargetNode->ComponentTemplate))
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Only scene components can be set as root"));
			return Result;
		}

		USCS_Node* CurrentRootNode = nullptr;
		SCS->GetSceneRootComponentTemplate(true, &CurrentRootNode);

		if (USCS_Node* CurrentParent = SCS->FindParentNode(TargetNode))
		{
			CurrentParent->RemoveChildNode(TargetNode);
			TargetNode->Modify();
			TargetNode->bIsParentComponentNative = false;
			TargetNode->ParentComponentOrVariableName = NAME_None;
			TargetNode->ParentComponentOwnerClassName = NAME_None;
			TargetNode->AttachToName = NAME_None;
		}

		if (!TargetNode->IsRootNode())
		{
			SCS->AddNode(TargetNode);
		}

		if (CurrentRootNode && CurrentRootNode != TargetNode)
		{
			if (CurrentRootNode->IsRootNode())
			{
				SCS->RemoveNode(CurrentRootNode, false);
			}
			TargetNode->AddChildNode(CurrentRootNode, true);
			CurrentRootNode->SetParent(TargetNode);
		}

		SCS->ValidateSceneRootNodes();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetObjectField(TEXT("component"), BuildComponentJson(Blueprint, SCS, TargetNode));
		Result->SetStringField(TEXT("root_component_name"), TargetNode->GetVariableName().ToString());
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleAttachComponent(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString ComponentName;
	FString ParentComponentName;
	FString SocketName;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("component_name"), ComponentName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'component_name'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("parent_component_name"), ParentComponentName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'parent_component_name'"));
	}
	Request.Params->TryGetStringField(TEXT("socket_name"), SocketName);

	auto Task = [BlueprintPath, ComponentName, ParentComponentName, SocketName]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return Result;
		}

		USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
		if (!SCS)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Blueprint does not have a SimpleConstructionScript"));
			return Result;
		}

		USCS_Node* ChildNode = SCS->FindSCSNode(FName(*ComponentName));
		USCS_Node* ParentNode = SCS->FindSCSNode(FName(*ParentComponentName));
		if (!ChildNode)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Component not found: %s"), *ComponentName));
			return Result;
		}
		if (!ParentNode)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Parent component not found: %s"), *ParentComponentName));
			return Result;
		}
		if (ChildNode == ParentNode)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Cannot attach a component to itself"));
			return Result;
		}
		if (!Cast<USceneComponent>(ChildNode->ComponentTemplate) || !Cast<USceneComponent>(ParentNode->ComponentTemplate))
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Only scene components can be attached"));
			return Result;
		}
		if (ParentNode->IsChildOf(ChildNode))
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Attachment would create a cycle in the component hierarchy"));
			return Result;
		}

		if (ChildNode->IsRootNode())
		{
			SCS->RemoveNode(ChildNode, false);
		}
		else if (USCS_Node* ExistingParent = SCS->FindParentNode(ChildNode))
		{
			ExistingParent->RemoveChildNode(ChildNode);
		}

		ParentNode->AddChildNode(ChildNode, true);
		ChildNode->SetParent(ParentNode);
		ChildNode->Modify();
		ChildNode->AttachToName = SocketName.IsEmpty() ? NAME_None : FName(*SocketName);

		SCS->ValidateSceneRootNodes();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetObjectField(TEXT("component"), BuildComponentJson(Blueprint, SCS, ChildNode));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleDetachComponent(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString ComponentName;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("component_name"), ComponentName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'component_name'"));
	}

	auto Task = [BlueprintPath, ComponentName]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return Result;
		}

		USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
		if (!SCS)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Blueprint does not have a SimpleConstructionScript"));
			return Result;
		}

		USCS_Node* Node = SCS->FindSCSNode(FName(*ComponentName));
		if (!Node)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Component not found: %s"), *ComponentName));
			return Result;
		}

		const bool bWasRoot = Node->IsRootNode();
		if (USCS_Node* ParentNode = SCS->FindParentNode(Node))
		{
			ParentNode->RemoveChildNode(Node);
		}

		Node->Modify();
		Node->bIsParentComponentNative = false;
		Node->ParentComponentOrVariableName = NAME_None;
		Node->ParentComponentOwnerClassName = NAME_None;
		Node->AttachToName = NAME_None;

		if (!bWasRoot)
		{
			SCS->AddNode(Node);
		}

		SCS->ValidateSceneRootNodes();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetObjectField(TEXT("component"), BuildComponentJson(Blueprint, SCS, Node));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleSetComponentProperty(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString ComponentName;
	FString PropertyName;
	FString ValueText;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("component_name"), ComponentName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'component_name'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("property_name"), PropertyName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'property_name'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("value"), ValueText))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'value'"));
	}

	auto Task = [BlueprintPath, ComponentName, PropertyName, ValueText]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return Result;
		}

		USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
		if (!SCS)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Blueprint does not have a SimpleConstructionScript"));
			return Result;
		}

		USCS_Node* Node = SCS->FindSCSNode(FName(*ComponentName));
		if (!Node || !Node->ComponentTemplate)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Component not found: %s"), *ComponentName));
			return Result;
		}

		UActorComponent* ComponentTemplate = Node->ComponentTemplate;
		FProperty* Property = FindPropertyByNameCaseInsensitive(ComponentTemplate->GetClass(), PropertyName);
		if (!Property)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Property not found on %s: %s"), *ComponentTemplate->GetClass()->GetName(), *PropertyName));
			return Result;
		}

		ComponentTemplate->Modify();
		void* PropertyValuePtr = Property->ContainerPtrToValuePtr<void>(ComponentTemplate);
		const TCHAR* ImportEnd = Property->ImportText_Direct(*ValueText, PropertyValuePtr, ComponentTemplate, PPF_None);
		if (ImportEnd == nullptr)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to parse value for property '%s'"), *PropertyName));
			return Result;
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

		FString ExportedValue;
		Property->ExportTextItem_Direct(ExportedValue, PropertyValuePtr, nullptr, ComponentTemplate, PPF_None);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetStringField(TEXT("component_name"), Node->GetVariableName().ToString());
		Result->SetStringField(TEXT("property_name"), Property->GetName());
		Result->SetStringField(TEXT("value"), ExportedValue);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleGetComponentProperty(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString ComponentName;
	FString PropertyName;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("component_name"), ComponentName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'component_name'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("property_name"), PropertyName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'property_name'"));
	}

	auto Task = [BlueprintPath, ComponentName, PropertyName]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return Result;
		}

		USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
		if (!SCS)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Blueprint does not have a SimpleConstructionScript"));
			return Result;
		}

		USCS_Node* Node = SCS->FindSCSNode(FName(*ComponentName));
		if (!Node || !Node->ComponentTemplate)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Component not found: %s"), *ComponentName));
			return Result;
		}

		UActorComponent* ComponentTemplate = Node->ComponentTemplate;
		FProperty* Property = FindPropertyByNameCaseInsensitive(ComponentTemplate->GetClass(), PropertyName);
		if (!Property)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Property not found on %s: %s"), *ComponentTemplate->GetClass()->GetName(), *PropertyName));
			return Result;
		}

		const void* PropertyValuePtr = Property->ContainerPtrToValuePtr<void>(ComponentTemplate);
		FString ExportedValue;
		Property->ExportTextItem_Direct(ExportedValue, PropertyValuePtr, nullptr, ComponentTemplate, PPF_None);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetStringField(TEXT("component_name"), Node->GetVariableName().ToString());
		Result->SetStringField(TEXT("property_name"), Property->GetName());
		Result->SetStringField(TEXT("value"), ExportedValue);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleSetComponentTransformDefault(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString ComponentName;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("component_name"), ComponentName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'component_name'"));
	}

	FVector RelativeLocation = FVector::ZeroVector;
	FRotator RelativeRotation = FRotator::ZeroRotator;
	FVector RelativeScale = FVector(1.0f, 1.0f, 1.0f);
	bool bHasLocation = false;
	bool bHasRotation = false;
	bool bHasScale = false;
	FString ParseError;

	if (!ParseVectorParam(Request.Params, TEXT("location"), RelativeLocation, ParseError, bHasLocation))
	{
		return InvalidParams(Request.Id, ParseError);
	}
	if (!ParseRotatorParam(Request.Params, TEXT("rotation"), RelativeRotation, ParseError, bHasRotation))
	{
		return InvalidParams(Request.Id, ParseError);
	}
	if (!ParseVectorParam(Request.Params, TEXT("scale"), RelativeScale, ParseError, bHasScale))
	{
		return InvalidParams(Request.Id, ParseError);
	}
	if (!bHasLocation && !bHasRotation && !bHasScale)
	{
		return InvalidParams(Request.Id, TEXT("At least one of 'location', 'rotation', or 'scale' is required"));
	}

	auto Task = [BlueprintPath, ComponentName, bHasLocation, RelativeLocation, bHasRotation, RelativeRotation, bHasScale, RelativeScale]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return Result;
		}

		USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
		if (!SCS)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Blueprint does not have a SimpleConstructionScript"));
			return Result;
		}

		USCS_Node* Node = SCS->FindSCSNode(FName(*ComponentName));
		if (!Node)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Component not found: %s"), *ComponentName));
			return Result;
		}

		USceneComponent* SceneTemplate = Cast<USceneComponent>(Node->ComponentTemplate);
		if (!SceneTemplate)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Component is not a scene component"));
			return Result;
		}

		SceneTemplate->Modify();
		if (bHasLocation)
		{
			SceneTemplate->SetRelativeLocation_Direct(RelativeLocation);
		}
		if (bHasRotation)
		{
			SceneTemplate->SetRelativeRotation_Direct(RelativeRotation);
		}
		if (bHasScale)
		{
			SceneTemplate->SetRelativeScale3D_Direct(RelativeScale);
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetObjectField(TEXT("component"), BuildComponentJson(Blueprint, SCS, Node));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleListFunctions(const FMCPRequest& Request)
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

		TArray<UEdGraph*> FunctionGraphs;
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph)
			{
				FunctionGraphs.Add(Graph);
			}
		}
		FunctionGraphs.Sort([](const UEdGraph& A, const UEdGraph& B)
		{
			return A.GetName().Compare(B.GetName(), ESearchCase::IgnoreCase) < 0;
		});

		TArray<TSharedPtr<FJsonValue>> FunctionsJson;
		for (UEdGraph* Graph : FunctionGraphs)
		{
			FunctionsJson.Add(MakeShared<FJsonValueObject>(BuildFunctionJson(Blueprint, Graph)));
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetArrayField(TEXT("functions"), FunctionsJson);
		Result->SetNumberField(TEXT("count"), FunctionsJson.Num());
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleCreateFunction(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString FunctionName;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("function_name"), FunctionName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'function_name'"));
	}

	auto Task = [BlueprintPath, FunctionName]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		auto Fail = [&Result](const FString& Error) -> TSharedPtr<FJsonObject>
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		};

		const FString TrimmedFunctionName = FunctionName.TrimStartAndEnd();
		if (TrimmedFunctionName.IsEmpty())
		{
			return Fail(TEXT("function_name must be non-empty"));
		}

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			return Fail(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		}

		if (ResolveGraph(Blueprint, TrimmedFunctionName))
		{
			return Fail(FString::Printf(TEXT("A graph already exists with name: %s"), *TrimmedFunctionName));
		}

		UEdGraph* NewGraph = UBlueprintEditorLibrary::AddFunctionGraph(Blueprint, TrimmedFunctionName);
		if (!NewGraph)
		{
			return Fail(FString::Printf(TEXT("Failed to create function: %s"), *TrimmedFunctionName));
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetObjectField(TEXT("function"), BuildFunctionJson(Blueprint, NewGraph));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleDeleteFunction(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString FunctionName;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("function_name"), FunctionName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'function_name'"));
	}

	auto Task = [BlueprintPath, FunctionName]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		auto Fail = [&Result](const FString& Error) -> TSharedPtr<FJsonObject>
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		};

		const FString TrimmedFunctionName = FunctionName.TrimStartAndEnd();
		if (TrimmedFunctionName.IsEmpty())
		{
			return Fail(TEXT("function_name must be non-empty"));
		}

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			return Fail(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		}

		UEdGraph* FunctionGraph = FindFunctionGraphByName(Blueprint, TrimmedFunctionName);
		if (!FunctionGraph)
		{
			return Fail(FString::Printf(TEXT("Function not found: %s"), *TrimmedFunctionName));
		}

		const FString RemovedFunctionName = FunctionGraph->GetName();
		FBlueprintEditorUtils::RemoveGraph(Blueprint, FunctionGraph, EGraphRemoveFlags::Default);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetStringField(TEXT("removed_function_name"), RemovedFunctionName);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleRenameFunction(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString FunctionName;
	FString NewFunctionName;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("function_name"), FunctionName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'function_name'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("new_function_name"), NewFunctionName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'new_function_name'"));
	}

	auto Task = [BlueprintPath, FunctionName, NewFunctionName]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		auto Fail = [&Result](const FString& Error) -> TSharedPtr<FJsonObject>
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		};

		const FString TrimmedFunctionName = FunctionName.TrimStartAndEnd();
		const FString TrimmedNewFunctionName = NewFunctionName.TrimStartAndEnd();
		if (TrimmedFunctionName.IsEmpty() || TrimmedNewFunctionName.IsEmpty())
		{
			return Fail(TEXT("function_name and new_function_name must be non-empty"));
		}

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			return Fail(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		}

		UEdGraph* FunctionGraph = FindFunctionGraphByName(Blueprint, TrimmedFunctionName);
		if (!FunctionGraph)
		{
			return Fail(FString::Printf(TEXT("Function not found: %s"), *TrimmedFunctionName));
		}

		if (FunctionGraph->GetName().Equals(TrimmedNewFunctionName, ESearchCase::IgnoreCase))
		{
			Result->SetBoolField(TEXT("success"), true);
			Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
			Result->SetBoolField(TEXT("changed"), false);
			Result->SetObjectField(TEXT("function"), BuildFunctionJson(Blueprint, FunctionGraph));
			return Result;
		}

		if (ResolveGraph(Blueprint, TrimmedNewFunctionName))
		{
			return Fail(FString::Printf(TEXT("A graph already exists with name: %s"), *TrimmedNewFunctionName));
		}

		const FString OldFunctionName = FunctionGraph->GetName();
		FBlueprintEditorUtils::RenameGraph(FunctionGraph, TrimmedNewFunctionName);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetStringField(TEXT("old_function_name"), OldFunctionName);
		Result->SetStringField(TEXT("new_function_name"), FunctionGraph->GetName());
		Result->SetBoolField(TEXT("changed"), true);
		Result->SetObjectField(TEXT("function"), BuildFunctionJson(Blueprint, FunctionGraph));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleSetFunctionFlags(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString FunctionName;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("function_name"), FunctionName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'function_name'"));
	}

	bool bPure = false;
	bool bConst = false;
	bool bCallInEditor = false;
	FString AccessName;
	const bool bHasPure = Request.Params->TryGetBoolField(TEXT("pure"), bPure);
	const bool bHasConst = Request.Params->TryGetBoolField(TEXT("const"), bConst);
	const bool bHasCallInEditor = Request.Params->TryGetBoolField(TEXT("call_in_editor"), bCallInEditor);
	const bool bHasAccess = Request.Params->TryGetStringField(TEXT("access"), AccessName);
	if (!bHasPure && !bHasConst && !bHasCallInEditor && !bHasAccess)
	{
		return InvalidParams(Request.Id, TEXT("Provide at least one of: pure, const, call_in_editor, access"));
	}

	uint32 ParsedAccessSpecifier = FUNC_Public;
	if (bHasAccess)
	{
		FString AccessError;
		if (!ParseAccessSpecifier(AccessName, ParsedAccessSpecifier, AccessError))
		{
			return InvalidParams(Request.Id, AccessError);
		}
	}

	auto Task = [BlueprintPath, FunctionName, bHasPure, bPure, bHasConst, bConst, bHasCallInEditor, bCallInEditor, bHasAccess, ParsedAccessSpecifier]() -> TSharedPtr<FJsonObject>
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

		UEdGraph* FunctionGraph = FindFunctionGraphByName(Blueprint, FunctionName);
		if (!FunctionGraph)
		{
			return Fail(FString::Printf(TEXT("Function not found: %s"), *FunctionName));
		}

		UK2Node_FunctionEntry* FunctionEntry = Cast<UK2Node_FunctionEntry>(FBlueprintEditorUtils::GetEntryNode(FunctionGraph));
		if (!FunctionEntry)
		{
			return Fail(TEXT("Could not resolve function entry node"));
		}

		FunctionEntry->Modify();
		const int32 ExistingExtraFlags = FunctionEntry->GetExtraFlags();
		int32 UpdatedExtraFlags = ExistingExtraFlags;
		bool bStructuralChange = false;
		bool bMetadataChange = false;

		if (bHasPure)
		{
			UpdatedExtraFlags = bPure ? (UpdatedExtraFlags | FUNC_BlueprintPure) : (UpdatedExtraFlags & ~FUNC_BlueprintPure);
		}
		if (bHasConst)
		{
			UpdatedExtraFlags = bConst ? (UpdatedExtraFlags | FUNC_Const) : (UpdatedExtraFlags & ~FUNC_Const);
		}
		if (bHasAccess)
		{
			UpdatedExtraFlags = (UpdatedExtraFlags & ~FUNC_AccessSpecifiers) | ParsedAccessSpecifier;
		}
		if (UpdatedExtraFlags != ExistingExtraFlags)
		{
			FunctionEntry->SetExtraFlags(UpdatedExtraFlags);
			bStructuralChange = true;
		}

		if (bHasCallInEditor && FunctionEntry->MetaData.bCallInEditor != bCallInEditor)
		{
			FunctionEntry->MetaData.bCallInEditor = bCallInEditor;
			bMetadataChange = true;
		}

		if (!bStructuralChange && !bMetadataChange)
		{
			return Fail(TEXT("No function flag changes were applied"));
		}

		if (Blueprint->SkeletonGeneratedClass)
		{
			if (UFunction* SkeletonFunction = Blueprint->SkeletonGeneratedClass->FindFunctionByName(FunctionGraph->GetFName()))
			{
				SkeletonFunction->Modify();
				uint32 UpdatedFunctionFlags = static_cast<uint32>(SkeletonFunction->FunctionFlags);
				if (bHasPure)
				{
					UpdatedFunctionFlags = bPure ? (UpdatedFunctionFlags | FUNC_BlueprintPure) : (UpdatedFunctionFlags & ~FUNC_BlueprintPure);
				}
				if (bHasConst)
				{
					UpdatedFunctionFlags = bConst ? (UpdatedFunctionFlags | FUNC_Const) : (UpdatedFunctionFlags & ~FUNC_Const);
				}
				if (bHasAccess)
				{
					UpdatedFunctionFlags = (UpdatedFunctionFlags & ~FUNC_AccessSpecifiers) | ParsedAccessSpecifier;
				}
				SkeletonFunction->FunctionFlags = static_cast<EFunctionFlags>(UpdatedFunctionFlags);
			}
		}

		if (bStructuralChange)
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		}
		else
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetBoolField(TEXT("structural_change"), bStructuralChange);
		Result->SetObjectField(TEXT("function"), BuildFunctionJson(Blueprint, FunctionGraph));
		return Result;
	};

TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleAddFunctionParam(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString FunctionName;
	FString ParamName;
	FString ParamType;
	FString DirectionName = TEXT("input");
	FString TypeReferencePath;
	FString ContainerTypeName;
	FString MapKeyTypeName;
	FString MapKeyTypeReferencePath;
	FString DefaultValue;
	bool bPassByReference = false;
	bool bConst = false;

	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("function_name"), FunctionName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'function_name'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("param_name"), ParamName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'param_name'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("param_type"), ParamType))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'param_type'"));
	}

	Request.Params->TryGetStringField(TEXT("direction"), DirectionName);
	Request.Params->TryGetStringField(TEXT("type_reference"), TypeReferencePath);
	const bool bHasContainerTypeOverride = Request.Params->TryGetStringField(TEXT("container_type"), ContainerTypeName);
	const bool bHasMapKeyType = Request.Params->TryGetStringField(TEXT("map_key_type"), MapKeyTypeName);
	Request.Params->TryGetStringField(TEXT("map_key_type_reference"), MapKeyTypeReferencePath);
	const bool bHasDefaultValue = Request.Params->TryGetStringField(TEXT("default_value"), DefaultValue);
	Request.Params->TryGetBoolField(TEXT("pass_by_reference"), bPassByReference);
	Request.Params->TryGetBoolField(TEXT("const"), bConst);

	EFunctionParamDirection Direction = EFunctionParamDirection::Input;
	FString DirectionError;
	if (!ParseFunctionParamDirection(DirectionName, Direction, DirectionError))
	{
		return InvalidParams(Request.Id, DirectionError);
	}

	EPinContainerType ContainerTypeOverride = EPinContainerType::None;
	if (bHasContainerTypeOverride)
	{
		FString ContainerError;
		if (!ParsePinContainerType(ContainerTypeName, ContainerTypeOverride, ContainerError))
		{
			return InvalidParams(Request.Id, ContainerError);
		}
	}

	auto Task = [BlueprintPath, FunctionName, ParamName, ParamType, Direction, TypeReferencePath, bHasContainerTypeOverride, ContainerTypeOverride, bHasMapKeyType, MapKeyTypeName, MapKeyTypeReferencePath, bPassByReference, bConst, bHasDefaultValue, DefaultValue]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		auto Fail = [&Result](const FString& Error) -> TSharedPtr<FJsonObject>
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		};

		const FString TrimmedParamName = ParamName.TrimStartAndEnd();
		if (TrimmedParamName.IsEmpty())
		{
			return Fail(TEXT("param_name must be non-empty"));
		}

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			return Fail(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		}

		UEdGraph* FunctionGraph = FindFunctionGraphByName(Blueprint, FunctionName);
		if (!FunctionGraph)
		{
			return Fail(FString::Printf(TEXT("Function not found: %s"), *FunctionName));
		}

		UK2Node_FunctionEntry* FunctionEntry = Cast<UK2Node_FunctionEntry>(FBlueprintEditorUtils::GetEntryNode(FunctionGraph));
		if (!FunctionEntry)
		{
			return Fail(TEXT("Could not resolve function entry node"));
		}

		FEdGraphPinType PinType;
		FString PinTypeError;
		if (!ResolvePinTypeFromTypeSpec(ParamType, TypeReferencePath, bHasContainerTypeOverride, ContainerTypeOverride, bHasMapKeyType, MapKeyTypeName, MapKeyTypeReferencePath, PinType, PinTypeError))
		{
			return Fail(PinTypeError);
		}

		const FName ParamFName(*TrimmedParamName);
		TArray<UK2Node_EditablePinBase*> UpdatedNodes;
		if (Direction == EFunctionParamDirection::Input)
		{
			for (const TSharedPtr<FUserPinInfo>& UserPin : FunctionEntry->UserDefinedPins)
			{
				if (UserPin.IsValid() && UserPin->PinName == ParamFName)
				{
					return Fail(FString::Printf(TEXT("Input parameter already exists: %s"), *TrimmedParamName));
				}
			}

			PinType.bIsReference = bPassByReference;
			PinType.bIsConst = bConst;
			UEdGraphPin* CreatedPin = FunctionEntry->CreateUserDefinedPin(ParamFName, PinType, EGPD_Output);
			if (!CreatedPin)
			{
				return Fail(FString::Printf(TEXT("Failed to add input parameter: %s"), *TrimmedParamName));
			}

			if (bHasDefaultValue)
			{
				for (const TSharedPtr<FUserPinInfo>& UserPin : FunctionEntry->UserDefinedPins)
				{
					if (UserPin.IsValid() && UserPin->PinName == ParamFName)
					{
						UserPin->PinDefaultValue = DefaultValue;
						break;
					}
				}
			}

			UpdatedNodes.Add(FunctionEntry);
		}
		else
		{
			PinType.bIsReference = false;
			PinType.bIsConst = bConst;

			UK2Node_FunctionResult* ResultNode = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(FunctionEntry);
			if (!ResultNode)
			{
				return Fail(TEXT("Failed to resolve function result node"));
			}

			TArray<UK2Node_EditablePinBase*> ResultNodes = GatherAllResultNodes(ResultNode);
			if (ResultNodes.Num() == 0)
			{
				return Fail(TEXT("No function result nodes found"));
			}

			for (const TSharedPtr<FUserPinInfo>& UserPin : ResultNodes[0]->UserDefinedPins)
			{
				if (UserPin.IsValid() && UserPin->PinName == ParamFName)
				{
					return Fail(FString::Printf(TEXT("Output parameter already exists: %s"), *TrimmedParamName));
				}
			}

			for (UK2Node_EditablePinBase* Node : ResultNodes)
			{
				UEdGraphPin* CreatedPin = Node ? Node->CreateUserDefinedPin(ParamFName, PinType, EGPD_Input, false) : nullptr;
				if (!CreatedPin)
				{
					return Fail(FString::Printf(TEXT("Failed to add output parameter: %s"), *TrimmedParamName));
				}
				UpdatedNodes.Add(Node);

				if (bHasDefaultValue)
				{
					for (const TSharedPtr<FUserPinInfo>& UserPin : Node->UserDefinedPins)
					{
						if (UserPin.IsValid() && UserPin->PinName == ParamFName)
						{
							UserPin->PinDefaultValue = DefaultValue;
							break;
						}
					}
				}
			}
		}

		for (UK2Node_EditablePinBase* Node : UpdatedNodes)
		{
			RefreshEditablePinNode(Node);
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetStringField(TEXT("function_name"), FunctionGraph->GetName());
		Result->SetStringField(TEXT("param_name"), ParamFName.ToString());
		Result->SetStringField(TEXT("direction"), FunctionParamDirectionToString(Direction));
		Result->SetObjectField(TEXT("function"), BuildFunctionJson(Blueprint, FunctionGraph));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleRemoveFunctionParam(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString FunctionName;
	FString ParamName;
	FString DirectionName;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("function_name"), FunctionName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'function_name'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("param_name"), ParamName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'param_name'"));
	}

	const bool bHasDirection = Request.Params->TryGetStringField(TEXT("direction"), DirectionName);
	EFunctionParamDirection Direction = EFunctionParamDirection::Input;
	if (bHasDirection)
	{
		FString DirectionError;
		if (!ParseFunctionParamDirection(DirectionName, Direction, DirectionError))
		{
			return InvalidParams(Request.Id, DirectionError);
		}
	}

	auto Task = [BlueprintPath, FunctionName, ParamName, bHasDirection, Direction]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		auto Fail = [&Result](const FString& Error) -> TSharedPtr<FJsonObject>
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		};

		const FString TrimmedParamName = ParamName.TrimStartAndEnd();
		if (TrimmedParamName.IsEmpty())
		{
			return Fail(TEXT("param_name must be non-empty"));
		}

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			return Fail(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		}

		UEdGraph* FunctionGraph = FindFunctionGraphByName(Blueprint, FunctionName);
		if (!FunctionGraph)
		{
			return Fail(FString::Printf(TEXT("Function not found: %s"), *FunctionName));
		}

		UK2Node_FunctionEntry* FunctionEntry = Cast<UK2Node_FunctionEntry>(FBlueprintEditorUtils::GetEntryNode(FunctionGraph));
		if (!FunctionEntry)
		{
			return Fail(TEXT("Could not resolve function entry node"));
		}

		const FName ParamFName(*TrimmedParamName);
		bool bRemovedInput = false;
		bool bRemovedOutput = false;
		TArray<UK2Node_EditablePinBase*> UpdatedNodes;

		const bool bAllowInput = !bHasDirection || Direction == EFunctionParamDirection::Input;
		const bool bAllowOutput = !bHasDirection || Direction == EFunctionParamDirection::Output;

		if (bAllowInput)
		{
			for (const TSharedPtr<FUserPinInfo>& UserPin : FunctionEntry->UserDefinedPins)
			{
				if (UserPin.IsValid() && UserPin->PinName == ParamFName)
				{
					FunctionEntry->Modify();
					FunctionEntry->RemoveUserDefinedPinByName(ParamFName);
					bRemovedInput = true;
					UpdatedNodes.AddUnique(FunctionEntry);
					break;
				}
			}
		}

		if (bAllowOutput)
		{
			TArray<UK2Node_FunctionResult*> FunctionResultNodes;
			FunctionGraph->GetNodesOfClass(FunctionResultNodes);
			if (FunctionResultNodes.Num() > 0)
			{
				TArray<UK2Node_EditablePinBase*> ResultNodes = GatherAllResultNodes(FunctionResultNodes[0]);
				for (UK2Node_EditablePinBase* Node : ResultNodes)
				{
					bool bNodeHadPin = false;
					for (const TSharedPtr<FUserPinInfo>& UserPin : Node->UserDefinedPins)
					{
						if (UserPin.IsValid() && UserPin->PinName == ParamFName)
						{
							bNodeHadPin = true;
							break;
						}
					}

					if (bNodeHadPin)
					{
						Node->Modify();
						Node->RemoveUserDefinedPinByName(ParamFName);
						bRemovedOutput = true;
						UpdatedNodes.AddUnique(Node);
					}
				}
			}
		}

		if (!bRemovedInput && !bRemovedOutput)
		{
			return Fail(FString::Printf(TEXT("Parameter not found: %s"), *TrimmedParamName));
		}

		for (UK2Node_EditablePinBase* Node : UpdatedNodes)
		{
			RefreshEditablePinNode(Node);
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		FString RemovedDirection = TEXT("input");
		if (bRemovedInput && bRemovedOutput)
		{
			RemovedDirection = TEXT("both");
		}
		else if (bRemovedOutput)
		{
			RemovedDirection = TEXT("output");
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetStringField(TEXT("function_name"), FunctionGraph->GetName());
		Result->SetStringField(TEXT("param_name"), ParamFName.ToString());
		Result->SetStringField(TEXT("removed_direction"), RemovedDirection);
		Result->SetObjectField(TEXT("function"), BuildFunctionJson(Blueprint, FunctionGraph));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleSetFunctionReturn(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString FunctionName;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("function_name"), FunctionName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'function_name'"));
	}

	bool bClear = false;
	FString ReturnName = UEdGraphSchema_K2::PN_ReturnValue.ToString();
	FString ReturnType;
	FString TypeReferencePath;
	FString ContainerTypeName;
	FString MapKeyTypeName;
	FString MapKeyTypeReferencePath;
	FString DefaultValue;

	Request.Params->TryGetBoolField(TEXT("clear"), bClear);
	Request.Params->TryGetStringField(TEXT("return_name"), ReturnName);
	const bool bHasReturnType = Request.Params->TryGetStringField(TEXT("return_type"), ReturnType);
	Request.Params->TryGetStringField(TEXT("type_reference"), TypeReferencePath);
	const bool bHasContainerTypeOverride = Request.Params->TryGetStringField(TEXT("container_type"), ContainerTypeName);
	const bool bHasMapKeyType = Request.Params->TryGetStringField(TEXT("map_key_type"), MapKeyTypeName);
	Request.Params->TryGetStringField(TEXT("map_key_type_reference"), MapKeyTypeReferencePath);
	const bool bHasDefaultValue = Request.Params->TryGetStringField(TEXT("default_value"), DefaultValue);

	if (!bClear && !bHasReturnType)
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'return_type' when clear=false"));
	}

	EPinContainerType ContainerTypeOverride = EPinContainerType::None;
	if (bHasContainerTypeOverride)
	{
		FString ContainerError;
		if (!ParsePinContainerType(ContainerTypeName, ContainerTypeOverride, ContainerError))
		{
			return InvalidParams(Request.Id, ContainerError);
		}
	}

	auto Task = [BlueprintPath, FunctionName, bClear, ReturnName, ReturnType, TypeReferencePath, bHasContainerTypeOverride, ContainerTypeOverride, bHasMapKeyType, MapKeyTypeName, MapKeyTypeReferencePath, bHasDefaultValue, DefaultValue]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		auto Fail = [&Result](const FString& Error) -> TSharedPtr<FJsonObject>
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		};

		FString TrimmedReturnName = ReturnName.TrimStartAndEnd();
		if (TrimmedReturnName.IsEmpty())
		{
			TrimmedReturnName = UEdGraphSchema_K2::PN_ReturnValue.ToString();
		}
		const FName ReturnFName(*TrimmedReturnName);

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			return Fail(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		}

		UEdGraph* FunctionGraph = FindFunctionGraphByName(Blueprint, FunctionName);
		if (!FunctionGraph)
		{
			return Fail(FString::Printf(TEXT("Function not found: %s"), *FunctionName));
		}

		UK2Node_FunctionEntry* FunctionEntry = Cast<UK2Node_FunctionEntry>(FBlueprintEditorUtils::GetEntryNode(FunctionGraph));
		if (!FunctionEntry)
		{
			return Fail(TEXT("Could not resolve function entry node"));
		}

		TArray<UK2Node_FunctionResult*> ExistingResultNodes;
		FunctionGraph->GetNodesOfClass(ExistingResultNodes);

		UK2Node_FunctionResult* ResultNode = ExistingResultNodes.Num() > 0 ? ExistingResultNodes[0] : nullptr;
		if (!ResultNode && !bClear)
		{
			ResultNode = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(FunctionEntry);
		}
		if (!ResultNode)
		{
			Result->SetBoolField(TEXT("success"), true);
			Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
			Result->SetStringField(TEXT("function_name"), FunctionGraph->GetName());
			Result->SetStringField(TEXT("return_name"), TrimmedReturnName);
			Result->SetBoolField(TEXT("changed"), false);
			Result->SetObjectField(TEXT("function"), BuildFunctionJson(Blueprint, FunctionGraph));
			return Result;
		}

		TArray<UK2Node_EditablePinBase*> ResultNodes = GatherAllResultNodes(ResultNode);
		if (ResultNodes.Num() == 0)
		{
			return Fail(TEXT("No function result nodes found"));
		}

		bool bRemovedExisting = false;
		for (UK2Node_EditablePinBase* Node : ResultNodes)
		{
			bool bHasPin = false;
			for (const TSharedPtr<FUserPinInfo>& UserPin : Node->UserDefinedPins)
			{
				if (UserPin.IsValid() && UserPin->PinName == ReturnFName)
				{
					bHasPin = true;
					break;
				}
			}
			if (bHasPin)
			{
				Node->Modify();
				Node->RemoveUserDefinedPinByName(ReturnFName);
				bRemovedExisting = true;
			}
		}

		bool bCreatedReturn = false;
		if (!bClear)
		{
			FEdGraphPinType ReturnPinType;
			FString PinTypeError;
			if (!ResolvePinTypeFromTypeSpec(ReturnType, TypeReferencePath, bHasContainerTypeOverride, ContainerTypeOverride, bHasMapKeyType, MapKeyTypeName, MapKeyTypeReferencePath, ReturnPinType, PinTypeError))
			{
				return Fail(PinTypeError);
			}
			ReturnPinType.bIsReference = false;
			ReturnPinType.bIsConst = false;

			for (UK2Node_EditablePinBase* Node : ResultNodes)
			{
				UEdGraphPin* NewPin = Node ? Node->CreateUserDefinedPin(ReturnFName, ReturnPinType, EGPD_Input, false) : nullptr;
				if (!NewPin)
				{
					return Fail(FString::Printf(TEXT("Failed to create return pin '%s'"), *TrimmedReturnName));
				}
				bCreatedReturn = true;

				if (bHasDefaultValue)
				{
					for (const TSharedPtr<FUserPinInfo>& UserPin : Node->UserDefinedPins)
					{
						if (UserPin.IsValid() && UserPin->PinName == ReturnFName)
						{
							UserPin->PinDefaultValue = DefaultValue;
							break;
						}
					}
				}
			}
		}

		if (!bRemovedExisting && !bCreatedReturn && bClear)
		{
			Result->SetBoolField(TEXT("success"), true);
			Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
			Result->SetStringField(TEXT("function_name"), FunctionGraph->GetName());
			Result->SetStringField(TEXT("return_name"), TrimmedReturnName);
			Result->SetBoolField(TEXT("changed"), false);
			Result->SetObjectField(TEXT("function"), BuildFunctionJson(Blueprint, FunctionGraph));
			return Result;
		}

		for (UK2Node_EditablePinBase* Node : ResultNodes)
		{
			RefreshEditablePinNode(Node);
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetStringField(TEXT("function_name"), FunctionGraph->GetName());
		Result->SetStringField(TEXT("return_name"), TrimmedReturnName);
		Result->SetBoolField(TEXT("changed"), true);
		Result->SetBoolField(TEXT("created"), bCreatedReturn);
		Result->SetObjectField(TEXT("function"), BuildFunctionJson(Blueprint, FunctionGraph));
		return Result;
	};

TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleListMacros(const FMCPRequest& Request)
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

		TArray<UEdGraph*> MacroGraphs;
		for (UEdGraph* Graph : Blueprint->MacroGraphs)
		{
			if (Graph)
			{
				MacroGraphs.Add(Graph);
			}
		}
		MacroGraphs.Sort([](const UEdGraph& A, const UEdGraph& B)
		{
			return A.GetName().Compare(B.GetName(), ESearchCase::IgnoreCase) < 0;
		});

		TArray<TSharedPtr<FJsonValue>> MacrosJson;
		for (UEdGraph* Graph : MacroGraphs)
		{
			TSharedPtr<FJsonObject> MacroObj = BuildGraphJson(Blueprint, Graph);
			MacroObj->SetStringField(TEXT("macro_name"), Graph->GetName());
			MacrosJson.Add(MakeShared<FJsonValueObject>(MacroObj));
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetArrayField(TEXT("macros"), MacrosJson);
		Result->SetNumberField(TEXT("count"), MacrosJson.Num());
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleCreateMacro(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString MacroName;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("macro_name"), MacroName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'macro_name'"));
	}

	auto Task = [BlueprintPath, MacroName]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		auto Fail = [&Result](const FString& Error) -> TSharedPtr<FJsonObject>
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		};

		const FString TrimmedMacroName = MacroName.TrimStartAndEnd();
		if (TrimmedMacroName.IsEmpty())
		{
			return Fail(TEXT("macro_name must be non-empty"));
		}

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			return Fail(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		}

		if (ResolveGraph(Blueprint, TrimmedMacroName))
		{
			return Fail(FString::Printf(TEXT("A graph already exists with name: %s"), *TrimmedMacroName));
		}

		UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
			Blueprint,
			FName(*TrimmedMacroName),
			UEdGraph::StaticClass(),
			UEdGraphSchema_K2::StaticClass()
		);
		if (!NewGraph)
		{
			return Fail(FString::Printf(TEXT("Failed to create macro: %s"), *TrimmedMacroName));
		}

		FBlueprintEditorUtils::AddMacroGraph(Blueprint, NewGraph, true, nullptr);

		TSharedPtr<FJsonObject> MacroObj = BuildGraphJson(Blueprint, NewGraph);
		MacroObj->SetStringField(TEXT("macro_name"), NewGraph->GetName());
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetObjectField(TEXT("macro"), MacroObj);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleDeleteMacro(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString MacroName;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("macro_name"), MacroName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'macro_name'"));
	}

	auto Task = [BlueprintPath, MacroName]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		auto Fail = [&Result](const FString& Error) -> TSharedPtr<FJsonObject>
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		};

		const FString TrimmedMacroName = MacroName.TrimStartAndEnd();
		if (TrimmedMacroName.IsEmpty())
		{
			return Fail(TEXT("macro_name must be non-empty"));
		}

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			return Fail(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		}

		UEdGraph* MacroGraph = FindMacroGraphByName(Blueprint, TrimmedMacroName);
		if (!MacroGraph)
		{
			return Fail(FString::Printf(TEXT("Macro not found: %s"), *TrimmedMacroName));
		}

		const FString RemovedMacroName = MacroGraph->GetName();
		FBlueprintEditorUtils::RemoveGraph(Blueprint, MacroGraph, EGraphRemoveFlags::Default);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetStringField(TEXT("removed_macro_name"), RemovedMacroName);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleListEventDispatchers(const FMCPRequest& Request)
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

		TArray<int32> DispatcherIndexes;
		for (int32 Index = 0; Index < Blueprint->NewVariables.Num(); ++Index)
		{
			if (Blueprint->NewVariables[Index].VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
			{
				DispatcherIndexes.Add(Index);
			}
		}
		DispatcherIndexes.Sort([Blueprint](const int32 A, const int32 B)
		{
			return Blueprint->NewVariables[A].VarName.ToString().Compare(Blueprint->NewVariables[B].VarName.ToString(), ESearchCase::IgnoreCase) < 0;
		});

		TArray<TSharedPtr<FJsonValue>> DispatchersJson;
		for (const int32 DispatcherIndex : DispatcherIndexes)
		{
			DispatchersJson.Add(MakeShared<FJsonValueObject>(BuildDispatcherJson(Blueprint, Blueprint->NewVariables[DispatcherIndex])));
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetArrayField(TEXT("event_dispatchers"), DispatchersJson);
		Result->SetNumberField(TEXT("count"), DispatchersJson.Num());
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleCreateEventDispatcher(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString DispatcherName;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("dispatcher_name"), DispatcherName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'dispatcher_name'"));
	}

	auto Task = [BlueprintPath, DispatcherName]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		auto Fail = [&Result](const FString& Error) -> TSharedPtr<FJsonObject>
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		};

		const FString TrimmedDispatcherName = DispatcherName.TrimStartAndEnd();
		if (TrimmedDispatcherName.IsEmpty())
		{
			return Fail(TEXT("dispatcher_name must be non-empty"));
		}

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			return Fail(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		}

		const FName DispatcherFName(*TrimmedDispatcherName);
		if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, DispatcherFName) != INDEX_NONE)
		{
			return Fail(FString::Printf(TEXT("A variable already exists with name: %s"), *TrimmedDispatcherName));
		}
		if (ResolveGraph(Blueprint, TrimmedDispatcherName))
		{
			return Fail(FString::Printf(TEXT("A graph already exists with name: %s"), *TrimmedDispatcherName));
		}

		FEdGraphPinType DelegatePinType;
		DelegatePinType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
		const bool bCreatedVariable = FBlueprintEditorUtils::AddMemberVariable(Blueprint, DispatcherFName, DelegatePinType);
		if (!bCreatedVariable)
		{
			return Fail(FString::Printf(TEXT("Failed to add dispatcher variable: %s"), *TrimmedDispatcherName));
		}

		const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
		if (!K2Schema)
		{
			FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, DispatcherFName);
			return Fail(TEXT("K2 schema unavailable"));
		}

		UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
			Blueprint,
			DispatcherFName,
			UEdGraph::StaticClass(),
			UEdGraphSchema_K2::StaticClass()
		);
		if (!NewGraph)
		{
			FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, DispatcherFName);
			return Fail(FString::Printf(TEXT("Failed to create signature graph for dispatcher: %s"), *TrimmedDispatcherName));
		}

		NewGraph->bEditable = false;
		K2Schema->CreateDefaultNodesForGraph(*NewGraph);
		K2Schema->CreateFunctionGraphTerminators(*NewGraph, static_cast<UClass*>(nullptr));
		K2Schema->AddExtraFunctionFlags(NewGraph, (FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public));
		K2Schema->MarkFunctionEntryAsEditable(NewGraph, true);
		Blueprint->DelegateSignatureGraphs.Add(NewGraph);

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		const int32 DispatcherIndex = FindDispatcherVariableIndex(Blueprint, DispatcherFName);
		if (DispatcherIndex == INDEX_NONE)
		{
			return Fail(TEXT("Dispatcher variable was created but could not be resolved"));
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetObjectField(TEXT("event_dispatcher"), BuildDispatcherJson(Blueprint, Blueprint->NewVariables[DispatcherIndex]));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleSetDispatcherSignature(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString DispatcherName;
	const TArray<TSharedPtr<FJsonValue>>* ParametersJson = nullptr;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("dispatcher_name"), DispatcherName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'dispatcher_name'"));
	}
	if (!Request.Params->TryGetArrayField(TEXT("parameters"), ParametersJson))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'parameters'"));
	}

	TArray<TSharedPtr<FJsonValue>> ParameterList = ParametersJson ? *ParametersJson : TArray<TSharedPtr<FJsonValue>>();
	auto Task = [BlueprintPath, DispatcherName, ParameterList]() -> TSharedPtr<FJsonObject>
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

		const FName DispatcherFName(*DispatcherName.TrimStartAndEnd());
		const int32 DispatcherIndex = FindDispatcherVariableIndex(Blueprint, DispatcherFName);
		if (DispatcherIndex == INDEX_NONE)
		{
			return Fail(FString::Printf(TEXT("Event dispatcher not found: %s"), *DispatcherName));
		}

		UEdGraph* SignatureGraph = FBlueprintEditorUtils::GetDelegateSignatureGraphByName(Blueprint, DispatcherFName);
		if (!SignatureGraph)
		{
			return Fail(FString::Printf(TEXT("Dispatcher signature graph not found: %s"), *DispatcherName));
		}

		UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(FBlueprintEditorUtils::GetEntryNode(SignatureGraph));
		if (!EntryNode)
		{
			return Fail(TEXT("Could not resolve dispatcher entry node"));
		}

		struct FRequestedSignatureParam
		{
			FName ParamName;
			FEdGraphPinType ParamType;
			bool bHasDefaultValue = false;
			FString DefaultValue;
		};

		TArray<FRequestedSignatureParam> RequestedParams;
		RequestedParams.Reserve(ParameterList.Num());
		TSet<FName> SeenParamNames;

		for (int32 ParamIndex = 0; ParamIndex < ParameterList.Num(); ++ParamIndex)
		{
			const TSharedPtr<FJsonValue>& ParamValue = ParameterList[ParamIndex];
			const TSharedPtr<FJsonObject> ParamObj = ParamValue.IsValid() ? ParamValue->AsObject() : nullptr;
			if (!ParamObj.IsValid())
			{
				return Fail(FString::Printf(TEXT("parameters[%d] must be an object"), ParamIndex));
			}

			FString ParamName;
			FString ParamTypeName;
			if (!ParamObj->TryGetStringField(TEXT("param_name"), ParamName))
			{
				return Fail(FString::Printf(TEXT("parameters[%d] is missing 'param_name'"), ParamIndex));
			}
			if (!ParamObj->TryGetStringField(TEXT("param_type"), ParamTypeName))
			{
				return Fail(FString::Printf(TEXT("parameters[%d] is missing 'param_type'"), ParamIndex));
			}

			const FString TrimmedParamName = ParamName.TrimStartAndEnd();
			if (TrimmedParamName.IsEmpty())
			{
				return Fail(FString::Printf(TEXT("parameters[%d].param_name must be non-empty"), ParamIndex));
			}

			const FName ParamFName(*TrimmedParamName);
			if (SeenParamNames.Contains(ParamFName))
			{
				return Fail(FString::Printf(TEXT("Duplicate parameter name: %s"), *TrimmedParamName));
			}
			SeenParamNames.Add(ParamFName);

			FString TypeReferencePath;
			ParamObj->TryGetStringField(TEXT("type_reference"), TypeReferencePath);

			FString ContainerTypeName;
			EPinContainerType ContainerTypeOverride = EPinContainerType::None;
			const bool bHasContainerTypeOverride = ParamObj->TryGetStringField(TEXT("container_type"), ContainerTypeName);
			if (bHasContainerTypeOverride)
			{
				FString ContainerError;
				if (!ParsePinContainerType(ContainerTypeName, ContainerTypeOverride, ContainerError))
				{
					return Fail(FString::Printf(TEXT("parameters[%d]: %s"), ParamIndex, *ContainerError));
				}
			}

			FString MapKeyTypeName;
			const bool bHasMapKeyType = ParamObj->TryGetStringField(TEXT("map_key_type"), MapKeyTypeName);
			FString MapKeyTypeReferencePath;
			ParamObj->TryGetStringField(TEXT("map_key_type_reference"), MapKeyTypeReferencePath);

			FEdGraphPinType PinType;
			FString PinTypeError;
			if (!ResolvePinTypeFromTypeSpec(ParamTypeName, TypeReferencePath, bHasContainerTypeOverride, ContainerTypeOverride, bHasMapKeyType, MapKeyTypeName, MapKeyTypeReferencePath, PinType, PinTypeError))
			{
				return Fail(FString::Printf(TEXT("parameters[%d]: %s"), ParamIndex, *PinTypeError));
			}

			bool bPassByReference = false;
			if (ParamObj->TryGetBoolField(TEXT("pass_by_reference"), bPassByReference))
			{
				PinType.bIsReference = bPassByReference;
			}

			bool bConst = false;
			if (ParamObj->TryGetBoolField(TEXT("const"), bConst))
			{
				PinType.bIsConst = bConst;
			}

			FRequestedSignatureParam& Requested = RequestedParams.AddDefaulted_GetRef();
			Requested.ParamName = ParamFName;
			Requested.ParamType = PinType;
			Requested.bHasDefaultValue = ParamObj->TryGetStringField(TEXT("default_value"), Requested.DefaultValue);
		}

		TArray<FName> ExistingParamNames;
		for (const TSharedPtr<FUserPinInfo>& UserPin : EntryNode->UserDefinedPins)
		{
			if (UserPin.IsValid())
			{
				ExistingParamNames.Add(UserPin->PinName);
			}
		}
		for (const FName ExistingParamName : ExistingParamNames)
		{
			EntryNode->RemoveUserDefinedPinByName(ExistingParamName);
		}

		for (const FRequestedSignatureParam& RequestedParam : RequestedParams)
		{
			UEdGraphPin* NewPin = EntryNode->CreateUserDefinedPin(RequestedParam.ParamName, RequestedParam.ParamType, EGPD_Output, false);
			if (!NewPin)
			{
				return Fail(FString::Printf(TEXT("Failed to add dispatcher parameter: %s"), *RequestedParam.ParamName.ToString()));
			}

			if (RequestedParam.bHasDefaultValue)
			{
				for (const TSharedPtr<FUserPinInfo>& UserPin : EntryNode->UserDefinedPins)
				{
					if (UserPin.IsValid() && UserPin->PinName == RequestedParam.ParamName)
					{
						UserPin->PinDefaultValue = RequestedParam.DefaultValue;
						break;
					}
				}
			}
		}

		RefreshEditablePinNode(EntryNode);
		FBlueprintEditorUtils::ConformDelegateSignatureGraphs(Blueprint);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetObjectField(TEXT("event_dispatcher"), BuildDispatcherJson(Blueprint, Blueprint->NewVariables[DispatcherIndex]));
		Result->SetNumberField(TEXT("signature_param_count"), RequestedParams.Num());
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

FMCPResponse FBlueprintService::HandleAddNodeByClass(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString NodeClassName;
	FString GraphName = TEXT("EventGraph");
	int32 NodeX = 0;
	int32 NodeY = 0;

	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("node_class"), NodeClassName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'node_class'"));
	}
	Request.Params->TryGetStringField(TEXT("graph_name"), GraphName);
	Request.Params->TryGetNumberField(TEXT("x"), NodeX);
	Request.Params->TryGetNumberField(TEXT("y"), NodeY);

	auto Task = [BlueprintPath, NodeClassName, GraphName, NodeX, NodeY]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		auto Fail = [&Result](const FString& Error) -> TSharedPtr<FJsonObject>
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		};

		const FString TrimmedClassName = NodeClassName.TrimStartAndEnd();
		if (TrimmedClassName.IsEmpty())
		{
			return Fail(TEXT("node_class must be non-empty"));
		}

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

		UClass* NodeClass = ResolveClassByNameOrPath(TrimmedClassName);
		if (!NodeClass)
		{
			return Fail(FString::Printf(TEXT("Node class not found: %s"), *TrimmedClassName));
		}
		if (!NodeClass->IsChildOf(UEdGraphNode::StaticClass()))
		{
			return Fail(FString::Printf(TEXT("Class is not a graph node type: %s"), *NodeClass->GetPathName()));
		}
		if (NodeClass->HasAnyClassFlags(CLASS_Abstract))
		{
			return Fail(FString::Printf(TEXT("Cannot instantiate abstract node class: %s"), *NodeClass->GetPathName()));
		}

		UEdGraphNode* SpawnedNode = SpawnNodeFromClass(Graph, NodeClass, NodeX, NodeY);
		if (!SpawnedNode)
		{
			return Fail(FString::Printf(TEXT("Failed to spawn node class: %s"), *NodeClass->GetPathName()));
		}

		if (const UK2Node* K2Node = Cast<UK2Node>(SpawnedNode); K2Node && K2Node->NodeCausesStructuralBlueprintChange())
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		}
		else
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetObjectField(TEXT("node"), BuildNodeJson(SpawnedNode));
		Result->SetStringField(TEXT("node_class"), NodeClass->GetPathName());
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleAddCustomEventNode(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString EventName;
	FString GraphName = TEXT("EventGraph");
	int32 NodeX = 0;
	int32 NodeY = 0;
	bool bCallInEditor = false;

	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("event_name"), EventName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'event_name'"));
	}
	Request.Params->TryGetStringField(TEXT("graph_name"), GraphName);
	Request.Params->TryGetNumberField(TEXT("x"), NodeX);
	Request.Params->TryGetNumberField(TEXT("y"), NodeY);
	Request.Params->TryGetBoolField(TEXT("call_in_editor"), bCallInEditor);

	auto Task = [BlueprintPath, EventName, GraphName, NodeX, NodeY, bCallInEditor]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		auto Fail = [&Result](const FString& Error) -> TSharedPtr<FJsonObject>
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		};

		const FString TrimmedEventName = EventName.TrimStartAndEnd();
		if (TrimmedEventName.IsEmpty())
		{
			return Fail(TEXT("event_name must be non-empty"));
		}

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

		FGraphNodeCreator<UK2Node_CustomEvent> EventCreator(*Graph);
		UK2Node_CustomEvent* EventNode = EventCreator.CreateNode();
		if (!EventNode)
		{
			return Fail(TEXT("Failed to create custom event node"));
		}

		EventNode->NodePosX = NodeX;
		EventNode->NodePosY = NodeY;
		EventCreator.Finalize();
		EventNode->OnRenameNode(TrimmedEventName);
		EventNode->bCallInEditor = bCallInEditor;
		EventNode->ReconstructNode();

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetObjectField(TEXT("node"), BuildNodeJson(EventNode));
		Result->SetStringField(TEXT("event_name"), EventNode->CustomFunctionName.ToString());
		Result->SetBoolField(TEXT("call_in_editor"), EventNode->bCallInEditor);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleAddCommentNode(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString GraphName = TEXT("EventGraph");
	FString CommentText = TEXT("Comment");
	int32 NodeX = 0;
	int32 NodeY = 0;
	int32 NodeWidth = 400;
	int32 NodeHeight = 200;

	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	Request.Params->TryGetStringField(TEXT("graph_name"), GraphName);
	Request.Params->TryGetStringField(TEXT("comment"), CommentText);
	Request.Params->TryGetNumberField(TEXT("x"), NodeX);
	Request.Params->TryGetNumberField(TEXT("y"), NodeY);
	Request.Params->TryGetNumberField(TEXT("width"), NodeWidth);
	Request.Params->TryGetNumberField(TEXT("height"), NodeHeight);

	auto Task = [BlueprintPath, GraphName, CommentText, NodeX, NodeY, NodeWidth, NodeHeight]() -> TSharedPtr<FJsonObject>
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

		FGraphNodeCreator<UEdGraphNode_Comment> CommentCreator(*Graph);
		UEdGraphNode_Comment* CommentNode = CommentCreator.CreateNode();
		if (!CommentNode)
		{
			return Fail(TEXT("Failed to create comment node"));
		}

		CommentNode->NodePosX = NodeX;
		CommentNode->NodePosY = NodeY;
		CommentNode->NodeWidth = FMath::Max(NodeWidth, 64);
		CommentNode->NodeHeight = FMath::Max(NodeHeight, 64);
		CommentNode->NodeComment = CommentText;
		CommentCreator.Finalize();

		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetObjectField(TEXT("node"), BuildNodeJson(CommentNode));
		Result->SetStringField(TEXT("comment"), CommentNode->NodeComment);
		Result->SetNumberField(TEXT("width"), CommentNode->NodeWidth);
		Result->SetNumberField(TEXT("height"), CommentNode->NodeHeight);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleAddRerouteNode(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString GraphName = TEXT("EventGraph");
	int32 NodeX = 0;
	int32 NodeY = 0;

	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	Request.Params->TryGetStringField(TEXT("graph_name"), GraphName);
	Request.Params->TryGetNumberField(TEXT("x"), NodeX);
	Request.Params->TryGetNumberField(TEXT("y"), NodeY);

	auto Task = [BlueprintPath, GraphName, NodeX, NodeY]() -> TSharedPtr<FJsonObject>
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

		FGraphNodeCreator<UK2Node_Knot> KnotCreator(*Graph);
		UK2Node_Knot* KnotNode = KnotCreator.CreateNode();
		if (!KnotNode)
		{
			return Fail(TEXT("Failed to create reroute node"));
		}

		KnotNode->NodePosX = NodeX;
		KnotNode->NodePosY = NodeY;
		KnotCreator.Finalize();
		KnotNode->ReconstructNode();

		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetObjectField(TEXT("node"), BuildNodeJson(KnotNode));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleDeleteNode(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString GraphName = TEXT("EventGraph");
	FString NodeId;

	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("node_id"), NodeId))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'node_id'"));
	}
	Request.Params->TryGetStringField(TEXT("graph_name"), GraphName);

	auto Task = [BlueprintPath, GraphName, NodeId]() -> TSharedPtr<FJsonObject>
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

		UEdGraphNode* Node = FindNodeById(Graph, NodeId);
		if (!Node)
		{
			return Fail(FString::Printf(TEXT("Node not found for node_id: %s"), *NodeId));
		}
		if (!Node->CanUserDeleteNode())
		{
			return Fail(FString::Printf(TEXT("Node cannot be deleted: %s"), *Node->GetNodeTitle(ENodeTitleType::ListView).ToString()));
		}

		const bool bStructural = (Cast<UK2Node>(Node) != nullptr) && Cast<UK2Node>(Node)->NodeCausesStructuralBlueprintChange();
		Node->Modify();
		Node->DestroyNode();

		if (bStructural)
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		}
		else
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("deleted_node_id"), NodeId);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleDuplicateNode(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString GraphName = TEXT("EventGraph");
	TArray<FString> NodeIds;
	FString NodeIdsError;
	int32 OffsetX = 40;
	int32 OffsetY = 40;

	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!ExtractStringArrayField(Request.Params, TEXT("node_ids"), NodeIds, NodeIdsError))
	{
		return InvalidParams(Request.Id, NodeIdsError);
	}
	Request.Params->TryGetStringField(TEXT("graph_name"), GraphName);
	Request.Params->TryGetNumberField(TEXT("offset_x"), OffsetX);
	Request.Params->TryGetNumberField(TEXT("offset_y"), OffsetY);

	auto Task = [BlueprintPath, GraphName, NodeIds, OffsetX, OffsetY]() -> TSharedPtr<FJsonObject>
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

		TArray<UEdGraphNode*> SourceNodes;
		FString ResolveError;
		if (!FindNodesByIds(Graph, NodeIds, SourceNodes, ResolveError))
		{
			return Fail(ResolveError);
		}

		TSet<UObject*> NodesToExport;
		for (UEdGraphNode* SourceNode : SourceNodes)
		{
			if (SourceNode)
			{
				NodesToExport.Add(SourceNode);
			}
		}
		if (NodesToExport.Num() == 0)
		{
			return Fail(TEXT("No nodes resolved for duplication"));
		}

		FString ExportedText;
		FEdGraphUtilities::ExportNodesToText(NodesToExport, ExportedText);
		if (ExportedText.IsEmpty())
		{
			return Fail(TEXT("Failed to export selected nodes"));
		}
		if (!FEdGraphUtilities::CanImportNodesFromText(Graph, ExportedText))
		{
			return Fail(TEXT("Selected nodes cannot be duplicated into target graph"));
		}

		TSet<UEdGraphNode*> ImportedSet;
		FEdGraphUtilities::ImportNodesFromText(Graph, ExportedText, ImportedSet);
		if (ImportedSet.Num() == 0)
		{
			return Fail(TEXT("Node duplication import produced no nodes"));
		}

		TArray<UEdGraphNode*> ImportedNodes;
		ImportedNodes.Reserve(ImportedSet.Num());
		bool bStructural = false;
		for (UEdGraphNode* ImportedNode : ImportedSet)
		{
			if (!ImportedNode)
			{
				continue;
			}

			ImportedNode->Modify();
			ImportedNode->NodePosX += OffsetX;
			ImportedNode->NodePosY += OffsetY;
			if (const UK2Node* ImportedK2Node = Cast<UK2Node>(ImportedNode); ImportedK2Node && ImportedK2Node->NodeCausesStructuralBlueprintChange())
			{
				bStructural = true;
			}
			ImportedNodes.Add(ImportedNode);
		}
		ImportedNodes.Sort([](const UEdGraphNode& A, const UEdGraphNode& B)
		{
			if (A.NodePosY == B.NodePosY)
			{
				return A.NodePosX < B.NodePosX;
			}
			return A.NodePosY < B.NodePosY;
		});

		if (bStructural)
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		}
		else
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetArrayField(TEXT("nodes"), BuildNodesJsonArray(ImportedNodes));
		Result->SetNumberField(TEXT("count"), ImportedNodes.Num());
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleMoveNode(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString GraphName = TEXT("EventGraph");
	FString NodeId;
	double XValue = 0.0;
	double YValue = 0.0;
	double DeltaXValue = 0.0;
	double DeltaYValue = 0.0;

	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("node_id"), NodeId))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'node_id'"));
	}
	Request.Params->TryGetStringField(TEXT("graph_name"), GraphName);

	const bool bHasX = Request.Params->TryGetNumberField(TEXT("x"), XValue);
	const bool bHasY = Request.Params->TryGetNumberField(TEXT("y"), YValue);
	const bool bHasDeltaX = Request.Params->TryGetNumberField(TEXT("delta_x"), DeltaXValue);
	const bool bHasDeltaY = Request.Params->TryGetNumberField(TEXT("delta_y"), DeltaYValue);
	if (!bHasX && !bHasY && !bHasDeltaX && !bHasDeltaY)
	{
		return InvalidParams(Request.Id, TEXT("Provide at least one of: x, y, delta_x, delta_y"));
	}

	auto Task = [BlueprintPath, GraphName, NodeId, bHasX, bHasY, bHasDeltaX, bHasDeltaY, XValue, YValue, DeltaXValue, DeltaYValue]() -> TSharedPtr<FJsonObject>
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

		UEdGraphNode* Node = FindNodeById(Graph, NodeId);
		if (!Node)
		{
			return Fail(FString::Printf(TEXT("Node not found for node_id: %s"), *NodeId));
		}

		int32 NewX = Node->NodePosX;
		int32 NewY = Node->NodePosY;
		if (bHasX)
		{
			NewX = FMath::RoundToInt(XValue);
		}
		if (bHasY)
		{
			NewY = FMath::RoundToInt(YValue);
		}
		if (bHasDeltaX)
		{
			NewX += FMath::RoundToInt(DeltaXValue);
		}
		if (bHasDeltaY)
		{
			NewY += FMath::RoundToInt(DeltaYValue);
		}

		Node->Modify();
		Node->NodePosX = NewX;
		Node->NodePosY = NewY;
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetObjectField(TEXT("node"), BuildNodeJson(Node));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleRenameNode(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString GraphName = TEXT("EventGraph");
	FString NodeId;
	FString NewName;

	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("node_id"), NodeId))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'node_id'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("new_name"), NewName))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'new_name'"));
	}
	Request.Params->TryGetStringField(TEXT("graph_name"), GraphName);

	auto Task = [BlueprintPath, GraphName, NodeId, NewName]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		auto Fail = [&Result](const FString& Error) -> TSharedPtr<FJsonObject>
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), Error);
			return Result;
		};

		const FString TrimmedName = NewName.TrimStartAndEnd();
		if (TrimmedName.IsEmpty())
		{
			return Fail(TEXT("new_name must be non-empty"));
		}

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

		UEdGraphNode* Node = FindNodeById(Graph, NodeId);
		if (!Node)
		{
			return Fail(FString::Printf(TEXT("Node not found for node_id: %s"), *NodeId));
		}
		if (!Node->GetCanRenameNode())
		{
			return Fail(FString::Printf(TEXT("Node does not support rename: %s"), *Node->GetClass()->GetName()));
		}

		Node->Modify();
		Node->OnRenameNode(TrimmedName);

		if (const UK2Node* K2Node = Cast<UK2Node>(Node); K2Node && K2Node->NodeCausesStructuralBlueprintChange())
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		}
		else
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetObjectField(TEXT("node"), BuildNodeJson(Node));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleSetNodeComment(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString GraphName = TEXT("EventGraph");
	FString NodeId;
	FString CommentText;
	bool bBubbleVisible = false;
	const bool bHasBubbleVisible = Request.Params->TryGetBoolField(TEXT("bubble_visible"), bBubbleVisible);

	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("node_id"), NodeId))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'node_id'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("comment"), CommentText))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'comment'"));
	}
	Request.Params->TryGetStringField(TEXT("graph_name"), GraphName);

	auto Task = [BlueprintPath, GraphName, NodeId, CommentText, bHasBubbleVisible, bBubbleVisible]() -> TSharedPtr<FJsonObject>
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

		UEdGraphNode* Node = FindNodeById(Graph, NodeId);
		if (!Node)
		{
			return Fail(FString::Printf(TEXT("Node not found for node_id: %s"), *NodeId));
		}

		Node->Modify();
		Node->NodeComment = CommentText;
		if (bHasBubbleVisible)
		{
			Node->bCommentBubbleVisible = bBubbleVisible;
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetObjectField(TEXT("node"), BuildNodeJson(Node));
		Result->SetStringField(TEXT("comment"), Node->NodeComment);
		Result->SetBoolField(TEXT("bubble_visible"), Node->bCommentBubbleVisible);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleCollapseNodesToFunction(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString GraphName = TEXT("EventGraph");
	FString FunctionName;
	TArray<FString> NodeIds;
	FString NodeIdsError;

	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!ExtractStringArrayField(Request.Params, TEXT("node_ids"), NodeIds, NodeIdsError))
	{
		return InvalidParams(Request.Id, NodeIdsError);
	}
	Request.Params->TryGetStringField(TEXT("graph_name"), GraphName);
	Request.Params->TryGetStringField(TEXT("function_name"), FunctionName);

	auto Task = [BlueprintPath, GraphName, FunctionName, NodeIds]() -> TSharedPtr<FJsonObject>
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

		UEdGraph* SourceGraph = ResolveGraph(Blueprint, GraphName);
		if (!SourceGraph)
		{
			return Fail(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
		}

		TArray<UEdGraphNode*> SourceNodes;
		FString ResolveError;
		if (!FindNodesByIds(SourceGraph, NodeIds, SourceNodes, ResolveError))
		{
			return Fail(ResolveError);
		}
		for (UEdGraphNode* SourceNode : SourceNodes)
		{
			if (SourceNode && !SourceNode->CanUserDeleteNode())
			{
				return Fail(FString::Printf(TEXT("Cannot collapse node that cannot be deleted: %s"), *SourceNode->GetNodeTitle(ENodeTitleType::ListView).ToString()));
			}
		}

		FString TargetFunctionName = FunctionName.TrimStartAndEnd();
		if (TargetFunctionName.IsEmpty())
		{
			TargetFunctionName = FBlueprintEditorUtils::FindUniqueKismetName(Blueprint, TEXT("CollapsedFunction")).ToString();
		}
		else if (ResolveGraph(Blueprint, TargetFunctionName))
		{
			return Fail(FString::Printf(TEXT("A graph already exists with name: %s"), *TargetFunctionName));
		}

		UEdGraph* FunctionGraph = UBlueprintEditorLibrary::AddFunctionGraph(Blueprint, TargetFunctionName);
		if (!FunctionGraph)
		{
			return Fail(FString::Printf(TEXT("Failed to create function graph: %s"), *TargetFunctionName));
		}

		TSet<UObject*> NodesToExport;
		for (UEdGraphNode* SourceNode : SourceNodes)
		{
			if (SourceNode)
			{
				NodesToExport.Add(SourceNode);
			}
		}

		FString ExportedText;
		FEdGraphUtilities::ExportNodesToText(NodesToExport, ExportedText);
		if (ExportedText.IsEmpty())
		{
			FBlueprintEditorUtils::RemoveGraph(Blueprint, FunctionGraph, EGraphRemoveFlags::Default);
			return Fail(TEXT("Failed to export selected nodes"));
		}
		if (!FEdGraphUtilities::CanImportNodesFromText(FunctionGraph, ExportedText))
		{
			FBlueprintEditorUtils::RemoveGraph(Blueprint, FunctionGraph, EGraphRemoveFlags::Default);
			return Fail(TEXT("Selected nodes cannot be collapsed to a function graph"));
		}

		TSet<UEdGraphNode*> ImportedSet;
		FEdGraphUtilities::ImportNodesFromText(FunctionGraph, ExportedText, ImportedSet);
		if (ImportedSet.Num() == 0)
		{
			FBlueprintEditorUtils::RemoveGraph(Blueprint, FunctionGraph, EGraphRemoveFlags::Default);
			return Fail(TEXT("Collapse import produced no nodes in function graph"));
		}

		const FVector2D SourceLocation = ComputeAverageNodeLocation(SourceNodes);
		for (UEdGraphNode* SourceNode : SourceNodes)
		{
			if (!SourceNode)
			{
				continue;
			}
			SourceNode->Modify();
			SourceNode->DestroyNode();
		}

		UK2Node_CallFunction* FunctionCallNode = SpawnSelfCallFunctionNode(SourceGraph, Blueprint, FName(*FunctionGraph->GetName()), SourceLocation);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetObjectField(TEXT("function"), BuildFunctionJson(Blueprint, FunctionGraph));
		Result->SetArrayField(TEXT("moved_nodes"), BuildNodesJsonArray(ImportedSet.Array()));
		Result->SetBoolField(TEXT("created_call_node"), FunctionCallNode != nullptr);
		if (FunctionCallNode)
		{
			Result->SetObjectField(TEXT("call_node"), BuildNodeJson(FunctionCallNode));
		}
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleCollapseNodesToMacro(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString GraphName = TEXT("EventGraph");
	FString MacroName;
	TArray<FString> NodeIds;
	FString NodeIdsError;

	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!ExtractStringArrayField(Request.Params, TEXT("node_ids"), NodeIds, NodeIdsError))
	{
		return InvalidParams(Request.Id, NodeIdsError);
	}
	Request.Params->TryGetStringField(TEXT("graph_name"), GraphName);
	Request.Params->TryGetStringField(TEXT("macro_name"), MacroName);

	auto Task = [BlueprintPath, GraphName, MacroName, NodeIds]() -> TSharedPtr<FJsonObject>
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

		UEdGraph* SourceGraph = ResolveGraph(Blueprint, GraphName);
		if (!SourceGraph)
		{
			return Fail(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
		}

		TArray<UEdGraphNode*> SourceNodes;
		FString ResolveError;
		if (!FindNodesByIds(SourceGraph, NodeIds, SourceNodes, ResolveError))
		{
			return Fail(ResolveError);
		}
		for (UEdGraphNode* SourceNode : SourceNodes)
		{
			if (SourceNode && !SourceNode->CanUserDeleteNode())
			{
				return Fail(FString::Printf(TEXT("Cannot collapse node that cannot be deleted: %s"), *SourceNode->GetNodeTitle(ENodeTitleType::ListView).ToString()));
			}
		}

		FString TargetMacroName = MacroName.TrimStartAndEnd();
		if (TargetMacroName.IsEmpty())
		{
			TargetMacroName = FBlueprintEditorUtils::FindUniqueKismetName(Blueprint, TEXT("CollapsedMacro")).ToString();
		}
		else if (ResolveGraph(Blueprint, TargetMacroName))
		{
			return Fail(FString::Printf(TEXT("A graph already exists with name: %s"), *TargetMacroName));
		}

		UEdGraph* MacroGraph = FBlueprintEditorUtils::CreateNewGraph(
			Blueprint,
			FName(*TargetMacroName),
			UEdGraph::StaticClass(),
			UEdGraphSchema_K2::StaticClass()
		);
		if (!MacroGraph)
		{
			return Fail(FString::Printf(TEXT("Failed to create macro graph: %s"), *TargetMacroName));
		}
		FBlueprintEditorUtils::AddMacroGraph(Blueprint, MacroGraph, true, nullptr);

		TSet<UObject*> NodesToExport;
		for (UEdGraphNode* SourceNode : SourceNodes)
		{
			if (SourceNode)
			{
				NodesToExport.Add(SourceNode);
			}
		}

		FString ExportedText;
		FEdGraphUtilities::ExportNodesToText(NodesToExport, ExportedText);
		if (ExportedText.IsEmpty())
		{
			FBlueprintEditorUtils::RemoveGraph(Blueprint, MacroGraph, EGraphRemoveFlags::Default);
			return Fail(TEXT("Failed to export selected nodes"));
		}
		if (!FEdGraphUtilities::CanImportNodesFromText(MacroGraph, ExportedText))
		{
			FBlueprintEditorUtils::RemoveGraph(Blueprint, MacroGraph, EGraphRemoveFlags::Default);
			return Fail(TEXT("Selected nodes cannot be collapsed to a macro graph"));
		}

		TSet<UEdGraphNode*> ImportedSet;
		FEdGraphUtilities::ImportNodesFromText(MacroGraph, ExportedText, ImportedSet);
		if (ImportedSet.Num() == 0)
		{
			FBlueprintEditorUtils::RemoveGraph(Blueprint, MacroGraph, EGraphRemoveFlags::Default);
			return Fail(TEXT("Collapse import produced no nodes in macro graph"));
		}

		const FVector2D SourceLocation = ComputeAverageNodeLocation(SourceNodes);
		for (UEdGraphNode* SourceNode : SourceNodes)
		{
			if (!SourceNode)
			{
				continue;
			}
			SourceNode->Modify();
			SourceNode->DestroyNode();
		}

		UK2Node_MacroInstance* MacroNode = SpawnMacroInstanceNode(SourceGraph, MacroGraph, SourceLocation);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		TSharedPtr<FJsonObject> MacroObj = BuildGraphJson(Blueprint, MacroGraph);
		MacroObj->SetStringField(TEXT("macro_name"), MacroGraph->GetName());
		Result->SetBoolField(TEXT("success"), true);
		Result->SetObjectField(TEXT("macro"), MacroObj);
		Result->SetArrayField(TEXT("moved_nodes"), BuildNodesJsonArray(ImportedSet.Array()));
		Result->SetBoolField(TEXT("created_macro_node"), MacroNode != nullptr);
		if (MacroNode)
		{
			Result->SetObjectField(TEXT("macro_node"), BuildNodeJson(MacroNode));
		}
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleListNodePins(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString GraphName = TEXT("EventGraph");
	FString NodeId;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("node_id"), NodeId))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'node_id'"));
	}
	Request.Params->TryGetStringField(TEXT("graph_name"), GraphName);

	auto Task = [BlueprintPath, GraphName, NodeId]() -> TSharedPtr<FJsonObject>
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

		TArray<const UEdGraphPin*> Pins = GatherNodePins(Node);
		Pins.Sort([](const UEdGraphPin& A, const UEdGraphPin& B)
		{
			return BuildPinPath(&A).Compare(BuildPinPath(&B), ESearchCase::IgnoreCase) < 0;
		});

		TArray<TSharedPtr<FJsonValue>> PinsJson;
		PinsJson.Reserve(Pins.Num());
		for (const UEdGraphPin* Pin : Pins)
		{
			if (!Pin)
			{
				continue;
			}
			PinsJson.Add(MakeShared<FJsonValueObject>(BuildPinJsonDetailed(Pin)));
		}

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("node_id"), NodeId);
		Result->SetArrayField(TEXT("pins"), PinsJson);
		Result->SetNumberField(TEXT("count"), PinsJson.Num());
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleDisconnectPins(const FMCPRequest& Request)
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
			Result->SetStringField(TEXT("error"), TEXT("Could not resolve from/to pin by name/path"));
			return Result;
		}

		if (!FromPin->LinkedTo.Contains(ToPin) && !ToPin->LinkedTo.Contains(FromPin))
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Pins are not currently connected"));
			return Result;
		}

		const UEdGraphSchema* Schema = FromPin->GetSchema();
		if (!Schema)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Graph schema unavailable"));
			return Result;
		}

		Schema->BreakSinglePinLink(FromPin, ToPin);
		FromNode->NodeConnectionListChanged();
		ToNode->NodeConnectionListChanged();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("from_node_id"), FromNodeId);
		Result->SetStringField(TEXT("from_pin"), BuildPinPath(FromPin));
		Result->SetStringField(TEXT("to_node_id"), ToNodeId);
		Result->SetStringField(TEXT("to_pin"), BuildPinPath(ToPin));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleBreakPinLinks(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString GraphName = TEXT("EventGraph");
	FString NodeId;
	FString PinName;
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
	Request.Params->TryGetStringField(TEXT("graph_name"), GraphName);

	auto Task = [BlueprintPath, GraphName, NodeId, PinName]() -> TSharedPtr<FJsonObject>
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
			Result->SetStringField(TEXT("error"), TEXT("Could not resolve pin by name/path"));
			return Result;
		}

		const int32 BrokenLinkCount = Pin->LinkedTo.Num();
		if (const UEdGraphSchema* Schema = Pin->GetSchema())
		{
			Schema->BreakPinLinks(*Pin, true);
		}
		else
		{
			Pin->BreakAllPinLinks();
		}

		Node->NodeConnectionListChanged();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("node_id"), NodeId);
		Result->SetStringField(TEXT("pin_name"), BuildPinPath(Pin));
		Result->SetNumberField(TEXT("broken_link_count"), BrokenLinkCount);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleBreakAllNodeLinks(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString GraphName = TEXT("EventGraph");
	FString NodeId;
	if (!Request.Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'blueprint_path'"));
	}
	if (!Request.Params->TryGetStringField(TEXT("node_id"), NodeId))
	{
		return InvalidParams(Request.Id, TEXT("Missing required parameter 'node_id'"));
	}
	Request.Params->TryGetStringField(TEXT("graph_name"), GraphName);

	auto Task = [BlueprintPath, GraphName, NodeId]() -> TSharedPtr<FJsonObject>
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

		int32 BrokenLinkCount = 0;
		for (const UEdGraphPin* Pin : GatherNodePins(Node))
		{
			if (Pin)
			{
				BrokenLinkCount += Pin->LinkedTo.Num();
			}
		}

		Node->BreakAllNodeLinks();
		Node->NodeConnectionListChanged();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("node_id"), NodeId);
		Result->SetNumberField(TEXT("broken_link_count"), BrokenLinkCount);
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleResetPinDefaultValue(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString GraphName = TEXT("EventGraph");
	FString NodeId;
	FString PinName;
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
	Request.Params->TryGetStringField(TEXT("graph_name"), GraphName);

	auto Task = [BlueprintPath, GraphName, NodeId, PinName]() -> TSharedPtr<FJsonObject>
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
			Result->SetStringField(TEXT("error"), TEXT("Could not resolve pin by name/path"));
			return Result;
		}

		if (const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>())
		{
			K2Schema->ResetPinToAutogeneratedDefaultValue(Pin, true);
		}
		else if (const UEdGraphSchema* Schema = Pin->GetSchema())
		{
			Schema->ResetPinToAutogeneratedDefaultValue(Pin, true);
		}
		Node->PinDefaultValueChanged(Pin);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("node_id"), NodeId);
		Result->SetStringField(TEXT("pin_name"), BuildPinPath(Pin));
		Result->SetStringField(TEXT("default_value"), Pin->GetDefaultAsString());
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleSplitStructPin(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString GraphName = TEXT("EventGraph");
	FString NodeId;
	FString PinName;
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
	Request.Params->TryGetStringField(TEXT("graph_name"), GraphName);

	auto Task = [BlueprintPath, GraphName, NodeId, PinName]() -> TSharedPtr<FJsonObject>
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

		UEdGraphNode* Node = FindNodeById(Graph, NodeId);
		if (!Node)
		{
			return Fail(TEXT("Could not resolve node by node_id"));
		}

		UEdGraphPin* Pin = FindPinByName(Node, PinName);
		if (!Pin)
		{
			return Fail(TEXT("Could not resolve pin by name/path"));
		}

		const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
		if (!K2Schema)
		{
			return Fail(TEXT("K2 schema unavailable"));
		}
		if (!K2Schema->CanSplitStructPin(*Pin))
		{
			return Fail(TEXT("Pin cannot be split"));
		}

		const FString PinPath = BuildPinPath(Pin);
		K2Schema->SplitPin(Pin, true);

		UEdGraphPin* SplitParentPin = FindPinByPathOrName(Node, PinPath);
		if (!SplitParentPin)
		{
			SplitParentPin = FindPinByPathOrName(Node, Pin->PinName.ToString());
		}
		if (!SplitParentPin)
		{
			return Fail(TEXT("Pin split succeeded but parent pin could not be resolved"));
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("node_id"), NodeId);
		Result->SetObjectField(TEXT("pin"), BuildPinJsonDetailed(SplitParentPin));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandleRecombineStructPin(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString GraphName = TEXT("EventGraph");
	FString NodeId;
	FString PinName;
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
	Request.Params->TryGetStringField(TEXT("graph_name"), GraphName);

	auto Task = [BlueprintPath, GraphName, NodeId, PinName]() -> TSharedPtr<FJsonObject>
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

		UEdGraphNode* Node = FindNodeById(Graph, NodeId);
		if (!Node)
		{
			return Fail(TEXT("Could not resolve node by node_id"));
		}

		UEdGraphPin* Pin = FindPinByName(Node, PinName);
		if (!Pin)
		{
			return Fail(TEXT("Could not resolve pin by name/path"));
		}

		const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
		if (!K2Schema)
		{
			return Fail(TEXT("K2 schema unavailable"));
		}

		UEdGraphPin* PinToRecombine = Pin;
		if (!K2Schema->CanRecombineStructPin(*PinToRecombine) && PinToRecombine->ParentPin && K2Schema->CanRecombineStructPin(*PinToRecombine->ParentPin))
		{
			PinToRecombine = PinToRecombine->ParentPin;
		}
		if (!K2Schema->CanRecombineStructPin(*PinToRecombine))
		{
			return Fail(TEXT("Pin cannot be recombined"));
		}

		FString RootPinName = PinToRecombine->PinName.ToString();
		if (PinToRecombine->ParentPin)
		{
			RootPinName = PinToRecombine->ParentPin->PinName.ToString();
		}

		K2Schema->RecombinePin(PinToRecombine);
		UEdGraphPin* RecombinedPin = FindPinByPathOrName(Node, RootPinName);
		if (!RecombinedPin)
		{
			return Fail(TEXT("Pin recombine succeeded but pin could not be resolved"));
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("node_id"), NodeId);
		Result->SetObjectField(TEXT("pin"), BuildPinJsonDetailed(RecombinedPin));
		return Result;
	};

	TSharedPtr<FJsonObject> Result = FGameThreadDispatcher::DispatchToGameThreadSyncWithReturn<TSharedPtr<FJsonObject>>(Task);
	return FMCPResponse::Success(Request.Id, Result);
}

FMCPResponse FBlueprintService::HandlePromotePinToVariable(const FMCPRequest& Request)
{
	if (!Request.Params.IsValid())
	{
		return InvalidParams(Request.Id, TEXT("Missing params object"));
	}

	FString BlueprintPath;
	FString GraphName = TEXT("EventGraph");
	FString NodeId;
	FString PinName;
	FString RequestedVariableName;
	bool bToMemberVariable = true;
	double RequestedX = 0.0;
	double RequestedY = 0.0;
	const bool bHasX = Request.Params->TryGetNumberField(TEXT("x"), RequestedX);
	const bool bHasY = Request.Params->TryGetNumberField(TEXT("y"), RequestedY);

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
	Request.Params->TryGetStringField(TEXT("graph_name"), GraphName);
	Request.Params->TryGetStringField(TEXT("variable_name"), RequestedVariableName);
	Request.Params->TryGetBoolField(TEXT("to_member_variable"), bToMemberVariable);

	auto Task = [BlueprintPath, GraphName, NodeId, PinName, RequestedVariableName, bToMemberVariable, bHasX, bHasY, RequestedX, RequestedY]() -> TSharedPtr<FJsonObject>
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

		UEdGraphNode* Node = FindNodeById(Graph, NodeId);
		if (!Node)
		{
			return Fail(TEXT("Could not resolve node by node_id"));
		}

		UEdGraphPin* Pin = FindPinByName(Node, PinName);
		if (!Pin)
		{
			return Fail(TEXT("Could not resolve pin by name/path"));
		}
		if (Pin->bOrphanedPin)
		{
			return Fail(TEXT("Cannot promote orphaned pin"));
		}

		const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
		if (!K2Schema)
		{
			return Fail(TEXT("K2 schema unavailable"));
		}
		if (!K2Schema->CanPromotePinToVariable(*Pin, bToMemberVariable))
		{
			return Fail(TEXT("Pin cannot be promoted to variable"));
		}

		const FString PinPath = BuildPinPath(Pin);
		const FString PinSimpleName = Pin->PinName.ToString();
		FEdGraphPinType VariablePinType = Pin->PinType;
		VariablePinType.bIsConst = false;
		VariablePinType.bIsReference = false;
		VariablePinType.bIsWeakPointer = false;

		FName VariableName = NAME_None;
		const FString TrimmedRequestedName = RequestedVariableName.TrimStartAndEnd();
		if (!TrimmedRequestedName.IsEmpty())
		{
			VariableName = FName(*TrimmedRequestedName);
		}
		else
		{
			VariableName = FBlueprintEditorUtils::FindUniqueKismetName(Blueprint, bToMemberVariable ? TEXT("NewVar") : TEXT("NewLocalVar"));
		}

		bool bAddedVariable = false;
		UEdGraph* FunctionGraph = nullptr;
		if (bToMemberVariable)
		{
			bAddedVariable = FBlueprintEditorUtils::AddMemberVariable(Blueprint, VariableName, VariablePinType, Pin->GetDefaultAsString());
		}
		else
		{
			if (!FBlueprintEditorUtils::DoesSupportLocalVariables(Graph))
			{
				return Fail(TEXT("Target graph does not support local variables"));
			}
			FunctionGraph = FBlueprintEditorUtils::GetTopLevelGraph(Graph);
			if (!FunctionGraph)
			{
				return Fail(TEXT("Could not resolve top-level graph for local variable"));
			}
			bAddedVariable = FBlueprintEditorUtils::AddLocalVariable(Blueprint, FunctionGraph, VariableName, VariablePinType, Pin->GetDefaultAsString());
		}

		if (!bAddedVariable)
		{
			return Fail(FString::Printf(TEXT("Failed to add variable '%s'"), *VariableName.ToString()));
		}

		UEdGraphPin* RefreshedPin = FindPinByPathOrName(Node, PinPath);
		if (!RefreshedPin)
		{
			RefreshedPin = FindPinByPathOrName(Node, PinSimpleName);
		}
		if (!RefreshedPin)
		{
			return Fail(TEXT("Pin promotion created variable but target pin could not be resolved"));
		}

		FVector2f NewNodePos;
		if (bHasX && bHasY)
		{
			NewNodePos = FVector2f(static_cast<float>(RequestedX), static_cast<float>(RequestedY));
		}
		else
		{
			NewNodePos.X = RefreshedPin->Direction == EGPD_Input ? Node->NodePosX - 200.0f : Node->NodePosX + 400.0f;
			NewNodePos.Y = static_cast<float>(Node->NodePosY);
		}

		FEdGraphSchemaAction_K2NewNode NodeInfo;
		if (RefreshedPin->Direction == EGPD_Input)
		{
			UK2Node_VariableGet* TemplateNode = NewObject<UK2Node_VariableGet>();
			if (bToMemberVariable)
			{
				TemplateNode->VariableReference.SetSelfMember(VariableName);
			}
			else
			{
				TemplateNode->VariableReference.SetLocalMember(
					VariableName,
					FunctionGraph->GetName(),
					FBlueprintEditorUtils::FindLocalVariableGuidByName(Blueprint, FunctionGraph, VariableName)
				);
			}
			NodeInfo.NodeTemplate = TemplateNode;
		}
		else
		{
			UK2Node_VariableSet* TemplateNode = NewObject<UK2Node_VariableSet>();
			if (bToMemberVariable)
			{
				TemplateNode->VariableReference.SetSelfMember(VariableName);
			}
			else
			{
				TemplateNode->VariableReference.SetLocalMember(
					VariableName,
					FunctionGraph->GetName(),
					FBlueprintEditorUtils::FindLocalVariableGuidByName(Blueprint, FunctionGraph, VariableName)
				);
			}
			NodeInfo.NodeTemplate = TemplateNode;
		}

		UEdGraphNode* PromotedNode = NodeInfo.PerformAction(Graph, RefreshedPin, NewNodePos, false);
		if (!PromotedNode)
		{
			return Fail(TEXT("Variable created but failed to spawn promoted variable node"));
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("node_id"), NodeId);
		Result->SetStringField(TEXT("pin_name"), BuildPinPath(RefreshedPin));
		Result->SetStringField(TEXT("variable_name"), VariableName.ToString());
		Result->SetStringField(TEXT("variable_scope"), bToMemberVariable ? TEXT("member") : TEXT("local"));
		Result->SetObjectField(TEXT("promoted_node"), BuildNodeJson(PromotedNode));
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
