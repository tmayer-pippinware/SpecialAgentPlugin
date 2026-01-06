// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Lighting Service
 * 
 * Spawn and configure lighting actors, build lightmaps.
 * Methods: spawn_light, set_light_intensity, set_light_color, build_lighting
 */
class SPECIALAGENT_API FLightingService : public IMCPService
{
public:
	FLightingService();
	virtual ~FLightingService() = default;

	// IMCPService interface
	virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
	virtual FString GetServiceDescription() const override;
	virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
	FMCPResponse HandleSpawnLight(const FMCPRequest& Request);
	FMCPResponse HandleSetLightIntensity(const FMCPRequest& Request);
	FMCPResponse HandleSetLightColor(const FMCPRequest& Request);
	FMCPResponse HandleBuildLighting(const FMCPRequest& Request);
	
	// Helper method for executing Python scripts
	FMCPResponse ExecutePythonFromParams(const FMCPRequest& Request);
};

