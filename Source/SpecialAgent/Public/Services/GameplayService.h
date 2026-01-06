// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Gameplay Service
 * 
 * Spawn gameplay-related actors like trigger volumes and player starts.
 * Methods: spawn_trigger_volume, spawn_player_start
 */
class SPECIALAGENT_API FGameplayService : public IMCPService
{
public:
	FGameplayService();
	virtual ~FGameplayService() = default;

	// IMCPService interface
	virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
	virtual FString GetServiceDescription() const override;
	virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
	FMCPResponse HandleSpawnTriggerVolume(const FMCPRequest& Request);
	FMCPResponse HandleSpawnPlayerStart(const FMCPRequest& Request);
	
	// Helper method for executing Python scripts
	FMCPResponse ExecutePythonFromParams(const FMCPRequest& Request);
};

