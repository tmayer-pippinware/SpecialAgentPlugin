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
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_VariableGet.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/EngineTypes.h"
#include "GameFramework/Actor.h"
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

	static bool BuildPinType(const FString& TypeName, FEdGraphPinType& OutType, FString& OutError)
	{
		const FString Normalized = TypeName.TrimStartAndEnd().ToLower();

		OutType = FEdGraphPinType();
		OutType.ContainerType = EPinContainerType::None;

		if (Normalized == TEXT("bool") || Normalized == TEXT("boolean"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
			return true;
		}
		if (Normalized == TEXT("int") || Normalized == TEXT("int32"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Int;
			return true;
		}
		if (Normalized == TEXT("int64"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Int64;
			return true;
		}
		if (Normalized == TEXT("float"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Real;
			OutType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
			return true;
		}
		if (Normalized == TEXT("double"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Real;
			OutType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
			return true;
		}
		if (Normalized == TEXT("name"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Name;
			return true;
		}
		if (Normalized == TEXT("string"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_String;
			return true;
		}
		if (Normalized == TEXT("text"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Text;
			return true;
		}
		if (Normalized == TEXT("vector"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
			return true;
		}
		if (Normalized == TEXT("rotator"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
			return true;
		}
		if (Normalized == TEXT("transform"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
			return true;
		}

		OutError = FString::Printf(
			TEXT("Unsupported variable_type '%s'. Supported: bool, int, int64, float, double, name, string, text, vector, rotator, transform"),
			*TypeName
		);
		return false;
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
		TypeParam->SetStringField(TEXT("description"), TEXT("Variable type: bool, int, int64, float, double, name, string, text, vector, rotator, transform."));
		Tool.Parameters->SetObjectField(TEXT("variable_type"), TypeParam);

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
	if (MethodName == TEXT("list_graph_nodes")) return HandleListGraphNodes(Request);
	if (MethodName == TEXT("create_variable")) return HandleCreateVariable(Request);
	if (MethodName == TEXT("add_event_node")) return HandleAddEventNode(Request);
	if (MethodName == TEXT("add_call_function_node")) return HandleAddCallFunctionNode(Request);
	if (MethodName == TEXT("add_variable_get_node")) return HandleAddVariableGetNode(Request);
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
	Request.Params->TryGetStringField(TEXT("default_value"), DefaultValue);

	auto Task = [BlueprintPath, VariableName, VariableType, DefaultValue]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		UBlueprint* Blueprint = LoadBlueprint(BlueprintPath);
		if (!Blueprint)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return Result;
		}

		FEdGraphPinType PinType;
		FString TypeError;
		if (!BuildPinType(VariableType, PinType, TypeError))
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TypeError);
			return Result;
		}

		const bool bAdded = FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*VariableName), PinType, DefaultValue);
		if (!bAdded)
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to add variable '%s'"), *VariableName));
			return Result;
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint_path"), NormalizeBlueprintPath(BlueprintPath));
		Result->SetStringField(TEXT("variable_name"), VariableName);
		Result->SetStringField(TEXT("variable_type"), VariableType);
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
