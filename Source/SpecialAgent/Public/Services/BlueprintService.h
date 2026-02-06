// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

class UBlueprint;
class UClass;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;

/**
 * Blueprint Service
 *
 * Native Blueprint authoring tools for node-based graph wiring.
 */
class SPECIALAGENT_API FBlueprintService : public IMCPService
{
public:
	FBlueprintService();
	virtual ~FBlueprintService() = default;

	// IMCPService interface
	virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
	virtual FString GetServiceDescription() const override;
	virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
	FMCPResponse HandleCreateBlueprint(const FMCPRequest& Request);
	FMCPResponse HandleDuplicateBlueprint(const FMCPRequest& Request);
	FMCPResponse HandleRenameBlueprint(const FMCPRequest& Request);
	FMCPResponse HandleDeleteBlueprint(const FMCPRequest& Request);
	FMCPResponse HandleSaveBlueprint(const FMCPRequest& Request);
	FMCPResponse HandleReparentBlueprint(const FMCPRequest& Request);
	FMCPResponse HandleGetBlueprintInfo(const FMCPRequest& Request);
	FMCPResponse HandleSetClassSettings(const FMCPRequest& Request);
	FMCPResponse HandleListGraphs(const FMCPRequest& Request);
	FMCPResponse HandleCreateGraph(const FMCPRequest& Request);
	FMCPResponse HandleRenameGraph(const FMCPRequest& Request);
	FMCPResponse HandleDeleteGraph(const FMCPRequest& Request);
	FMCPResponse HandleSetGraphMetadata(const FMCPRequest& Request);
	FMCPResponse HandleFormatGraph(const FMCPRequest& Request);
	FMCPResponse HandleListGraphNodes(const FMCPRequest& Request);
	FMCPResponse HandleCreateVariable(const FMCPRequest& Request);
	FMCPResponse HandleListVariables(const FMCPRequest& Request);
	FMCPResponse HandleRenameVariable(const FMCPRequest& Request);
	FMCPResponse HandleDeleteVariable(const FMCPRequest& Request);
	FMCPResponse HandleSetVariableDefault(const FMCPRequest& Request);
	FMCPResponse HandleSetVariableMetadata(const FMCPRequest& Request);
	FMCPResponse HandleSetVariableInstanceEditable(const FMCPRequest& Request);
	FMCPResponse HandleSetVariableExposeOnSpawn(const FMCPRequest& Request);
	FMCPResponse HandleSetVariableSaveGame(const FMCPRequest& Request);
	FMCPResponse HandleSetVariableTransient(const FMCPRequest& Request);
	FMCPResponse HandleSetVariableReplication(const FMCPRequest& Request);
	FMCPResponse HandleListComponents(const FMCPRequest& Request);
	FMCPResponse HandleAddComponent(const FMCPRequest& Request);
	FMCPResponse HandleRemoveComponent(const FMCPRequest& Request);
	FMCPResponse HandleRenameComponent(const FMCPRequest& Request);
	FMCPResponse HandleSetRootComponent(const FMCPRequest& Request);
	FMCPResponse HandleAttachComponent(const FMCPRequest& Request);
	FMCPResponse HandleDetachComponent(const FMCPRequest& Request);
	FMCPResponse HandleSetComponentProperty(const FMCPRequest& Request);
	FMCPResponse HandleGetComponentProperty(const FMCPRequest& Request);
	FMCPResponse HandleSetComponentTransformDefault(const FMCPRequest& Request);
	FMCPResponse HandleListFunctions(const FMCPRequest& Request);
	FMCPResponse HandleCreateFunction(const FMCPRequest& Request);
	FMCPResponse HandleDeleteFunction(const FMCPRequest& Request);
	FMCPResponse HandleRenameFunction(const FMCPRequest& Request);
	FMCPResponse HandleSetFunctionFlags(const FMCPRequest& Request);
	FMCPResponse HandleAddFunctionParam(const FMCPRequest& Request);
	FMCPResponse HandleRemoveFunctionParam(const FMCPRequest& Request);
	FMCPResponse HandleSetFunctionReturn(const FMCPRequest& Request);
	FMCPResponse HandleListMacros(const FMCPRequest& Request);
	FMCPResponse HandleCreateMacro(const FMCPRequest& Request);
	FMCPResponse HandleDeleteMacro(const FMCPRequest& Request);
	FMCPResponse HandleListEventDispatchers(const FMCPRequest& Request);
	FMCPResponse HandleCreateEventDispatcher(const FMCPRequest& Request);
	FMCPResponse HandleSetDispatcherSignature(const FMCPRequest& Request);
	FMCPResponse HandleAddEventNode(const FMCPRequest& Request);
	FMCPResponse HandleAddCallFunctionNode(const FMCPRequest& Request);
	FMCPResponse HandleAddVariableGetNode(const FMCPRequest& Request);
	FMCPResponse HandleAddVariableSetNode(const FMCPRequest& Request);
	FMCPResponse HandleSetPinDefaultValue(const FMCPRequest& Request);
	FMCPResponse HandleConnectPins(const FMCPRequest& Request);
	FMCPResponse HandleCompileBlueprint(const FMCPRequest& Request);

	static FString NormalizeBlueprintPath(const FString& BlueprintPath);
	static FString NormalizeBlueprintAssetPath(const FString& BlueprintPath);
	static UBlueprint* LoadBlueprint(const FString& BlueprintPath);
	static UClass* ResolveClass(const FString& ClassNameOrPath);
	static UEdGraph* ResolveGraph(UBlueprint* Blueprint, const FString& GraphName);
	static UEdGraphNode* FindNodeById(UEdGraph* Graph, const FString& NodeId);
	static UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FString& PinName);
};
