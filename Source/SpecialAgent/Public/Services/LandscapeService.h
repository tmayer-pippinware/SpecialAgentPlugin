// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Landscape Service
 * 
 * Terrain sculpting and material layer painting.
 * Methods: get_info, sculpt_height, flatten_area, smooth_area, paint_layer
 */
class SPECIALAGENT_API FLandscapeService : public IMCPService
{
public:
	FLandscapeService();
	virtual ~FLandscapeService() = default;

	// IMCPService interface
	virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
	virtual FString GetServiceDescription() const override;
	virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
	FMCPResponse HandleGetInfo(const FMCPRequest& Request);
	FMCPResponse HandleSculptHeight(const FMCPRequest& Request);
	FMCPResponse HandleFlattenArea(const FMCPRequest& Request);
	FMCPResponse HandleSmoothArea(const FMCPRequest& Request);
	FMCPResponse HandlePaintLayer(const FMCPRequest& Request);
	
	// Helper method for executing Python scripts
	FMCPResponse ExecutePythonFromParams(const FMCPRequest& Request);
};

