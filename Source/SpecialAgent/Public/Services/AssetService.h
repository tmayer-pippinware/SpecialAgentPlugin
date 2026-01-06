// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Asset Service
 * 
 * Handles asset discovery and management operations.
 * Methods: list, find, get_properties, search
 */
class SPECIALAGENT_API FAssetService : public IMCPService
{
public:
	FAssetService();
	virtual ~FAssetService() = default;

	// IMCPService interface
	virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
	virtual FString GetServiceDescription() const override;
	virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
	// Method handlers
	FMCPResponse HandleListAssets(const FMCPRequest& Request);
	FMCPResponse HandleFindAsset(const FMCPRequest& Request);
	FMCPResponse HandleGetAssetProperties(const FMCPRequest& Request);
	FMCPResponse HandleSearchAssets(const FMCPRequest& Request);
	FMCPResponse HandleGetAssetBounds(const FMCPRequest& Request);
	FMCPResponse HandleGetAssetInfo(const FMCPRequest& Request);
};

