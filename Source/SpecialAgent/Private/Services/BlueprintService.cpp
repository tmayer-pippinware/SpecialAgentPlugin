// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/BlueprintService.h"

#include "GameThreadDispatcher.h"
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
	FString Normalized = BlueprintPath.TrimStartAndEnd();
	if (!Normalized.Contains(TEXT(".")))
	{
		const FString AssetName = FPackageName::GetShortName(Normalized);
		Normalized = FString::Printf(TEXT("%s.%s"), *Normalized, *AssetName);
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
