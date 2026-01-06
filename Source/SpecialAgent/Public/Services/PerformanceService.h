// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Performance Service
 * 
 * Level performance analysis and optimization tools.
 * Methods: get_statistics, get_actor_bounds, check_overlaps
 */
class SPECIALAGENT_API FPerformanceService : public IMCPService
{
public:
	FPerformanceService();
	virtual ~FPerformanceService() = default;

	// IMCPService interface
	virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
	virtual FString GetServiceDescription() const override;
	virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
	FMCPResponse HandleGetStatistics(const FMCPRequest& Request);
	FMCPResponse HandleGetActorBounds(const FMCPRequest& Request);
	FMCPResponse HandleCheckOverlaps(const FMCPRequest& Request);
	
	// Helper method for executing Python scripts
	FMCPResponse ExecutePythonFromParams(const FMCPRequest& Request);
};

