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
	FMCPResponse HandleCapabilities(const FMCPRequest& Request);
};
