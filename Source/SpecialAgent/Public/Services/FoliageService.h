// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Foliage Service
 * 
 * Procedural foliage painting and management.
 * Methods: paint_in_area, remove_from_area, get_density
 */
class SPECIALAGENT_API FFoliageService : public IMCPService
{
public:
	FFoliageService();
	virtual ~FFoliageService() = default;

	// IMCPService interface
	virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
	virtual FString GetServiceDescription() const override;
	virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
	FMCPResponse HandlePaintInArea(const FMCPRequest& Request);
	FMCPResponse HandleRemoveFromArea(const FMCPRequest& Request);
	FMCPResponse HandleGetDensity(const FMCPRequest& Request);
	
	// Helper method for executing Python scripts
	FMCPResponse ExecutePythonFromParams(const FMCPRequest& Request);
};

