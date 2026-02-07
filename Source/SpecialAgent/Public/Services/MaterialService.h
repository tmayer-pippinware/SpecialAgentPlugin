// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Material Service
 *
 * Baseline scaffold for native material authoring tools.
 */
class SPECIALAGENT_API FMaterialService : public IMCPService
{
public:
	FMaterialService();
	virtual ~FMaterialService() = default;

	// IMCPService interface
	virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
	virtual FString GetServiceDescription() const override;
	virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
	FMCPResponse HandleCreateMaterial(const FMCPRequest& Request);
	FMCPResponse HandleCreateMaterialInstance(const FMCPRequest& Request);
	FMCPResponse HandleCreateMaterialFunction(const FMCPRequest& Request);
	FMCPResponse HandleCreateParameterCollection(const FMCPRequest& Request);
	FMCPResponse HandleDuplicateAsset(const FMCPRequest& Request);
	FMCPResponse HandleRenameAsset(const FMCPRequest& Request);
	FMCPResponse HandleDeleteAsset(const FMCPRequest& Request);
	FMCPResponse HandleSaveAsset(const FMCPRequest& Request);
	FMCPResponse HandleGetMaterialInfo(const FMCPRequest& Request);
	FMCPResponse HandleSetMaterialSettings(const FMCPRequest& Request);
	FMCPResponse HandleListNodes(const FMCPRequest& Request);
	FMCPResponse HandleAddExpressionByClass(const FMCPRequest& Request);
	FMCPResponse HandleDeleteNode(const FMCPRequest& Request);
	FMCPResponse HandleDuplicateNode(const FMCPRequest& Request);
	FMCPResponse HandleMoveNode(const FMCPRequest& Request);
	FMCPResponse HandleAddCommentNode(const FMCPRequest& Request);
	FMCPResponse HandleAddRerouteNode(const FMCPRequest& Request);
	FMCPResponse HandleLayoutGraph(const FMCPRequest& Request);
	FMCPResponse HandleListNodePins(const FMCPRequest& Request);
	FMCPResponse HandleConnectPins(const FMCPRequest& Request);
	FMCPResponse HandleDisconnectPins(const FMCPRequest& Request);
	FMCPResponse HandleBreakPinLinks(const FMCPRequest& Request);
	FMCPResponse HandleBreakAllNodeLinks(const FMCPRequest& Request);
	FMCPResponse HandleSetPinDefaultValue(const FMCPRequest& Request);
	FMCPResponse HandleResetPinDefaultValue(const FMCPRequest& Request);
	FMCPResponse HandleSetMaterialOutput(const FMCPRequest& Request);
	FMCPResponse HandleClearMaterialOutput(const FMCPRequest& Request);
	FMCPResponse HandleListConnectedOutputs(const FMCPRequest& Request);
	FMCPResponse HandleSetCustomUVOutput(const FMCPRequest& Request);
	FMCPResponse HandleSetPixelDepthOffsetOutput(const FMCPRequest& Request);
	FMCPResponse HandleListParameters(const FMCPRequest& Request);
	FMCPResponse HandleAddParameter(const FMCPRequest& Request);
	FMCPResponse HandleRemoveParameter(const FMCPRequest& Request);
	FMCPResponse HandleRenameParameter(const FMCPRequest& Request);
	FMCPResponse HandleSetParameterDefault(const FMCPRequest& Request);
	FMCPResponse HandleSetParameterMetadata(const FMCPRequest& Request);
	FMCPResponse HandleSetParameterChannelNames(const FMCPRequest& Request);
	FMCPResponse HandleCapabilities(const FMCPRequest& Request);
};
