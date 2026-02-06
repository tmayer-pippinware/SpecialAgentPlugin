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
	FMCPResponse HandleListGraphNodes(const FMCPRequest& Request);
	FMCPResponse HandleCreateVariable(const FMCPRequest& Request);
	FMCPResponse HandleAddEventNode(const FMCPRequest& Request);
	FMCPResponse HandleAddCallFunctionNode(const FMCPRequest& Request);
	FMCPResponse HandleAddVariableGetNode(const FMCPRequest& Request);
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
