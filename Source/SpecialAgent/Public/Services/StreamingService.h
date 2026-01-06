// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Streaming Service
 * 
 * Level streaming management for large worlds.
 * Methods: list_levels, load_level, unload_level, set_level_visibility
 */
class SPECIALAGENT_API FStreamingService : public IMCPService
{
public:
	FStreamingService();
	virtual ~FStreamingService() = default;

	// IMCPService interface
	virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
	virtual FString GetServiceDescription() const override;
	virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
	FMCPResponse HandleListLevels(const FMCPRequest& Request);
	FMCPResponse HandleLoadLevel(const FMCPRequest& Request);
	FMCPResponse HandleUnloadLevel(const FMCPRequest& Request);
	FMCPResponse HandleSetLevelVisibility(const FMCPRequest& Request);
	
	// Helper method for executing Python scripts
	FMCPResponse ExecutePythonFromParams(const FMCPRequest& Request);
};

