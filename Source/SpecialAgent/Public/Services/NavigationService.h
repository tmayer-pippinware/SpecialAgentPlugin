// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Navigation Service
 * 
 * Navigation mesh management and pathfinding testing.
 * Methods: rebuild_navmesh, test_path
 */
class SPECIALAGENT_API FNavigationService : public IMCPService
{
public:
	FNavigationService();
	virtual ~FNavigationService() = default;

	// IMCPService interface
	virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
	virtual FString GetServiceDescription() const override;
	virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
	FMCPResponse HandleRebuildNavMesh(const FMCPRequest& Request);
	FMCPResponse HandleTestPath(const FMCPRequest& Request);
	
	// Helper method for executing Python scripts
	FMCPResponse ExecutePythonFromParams(const FMCPRequest& Request);
};

