// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * World Service
 * 
 * Handles world/level introspection and actor manipulation.
 * 30+ methods for querying, spawning, modifying, and organizing actors.
 */
class SPECIALAGENT_API FWorldService : public IMCPService
{
public:
	FWorldService();
	virtual ~FWorldService() = default;

	// IMCPService interface
	virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
	virtual FString GetServiceDescription() const override;
	virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
	// Query methods
	FMCPResponse HandleListActors(const FMCPRequest& Request);
	FMCPResponse HandleGetActor(const FMCPRequest& Request);
	FMCPResponse HandleFindActorsByTag(const FMCPRequest& Request);
	FMCPResponse HandleGetLevelInfo(const FMCPRequest& Request);

	// Spawn/Delete methods
	FMCPResponse HandleSpawnActor(const FMCPRequest& Request);
	FMCPResponse HandleSpawnActorsBatch(const FMCPRequest& Request);
	FMCPResponse HandleDeleteActor(const FMCPRequest& Request);
	FMCPResponse HandleDeleteActorsBatch(const FMCPRequest& Request);
	FMCPResponse HandleDuplicateActor(const FMCPRequest& Request);

	// Transform methods
	FMCPResponse HandleSetActorTransform(const FMCPRequest& Request);
	FMCPResponse HandleSetActorLocation(const FMCPRequest& Request);
	FMCPResponse HandleSetActorRotation(const FMCPRequest& Request);
	FMCPResponse HandleSetActorScale(const FMCPRequest& Request);

	// Property methods
	FMCPResponse HandleSetActorProperty(const FMCPRequest& Request);
	FMCPResponse HandleSetActorLabel(const FMCPRequest& Request);
	FMCPResponse HandleSetActorMaterial(const FMCPRequest& Request);
	FMCPResponse HandleSetMaterialParameter(const FMCPRequest& Request);

	// Organization methods
	FMCPResponse HandleCreateFolder(const FMCPRequest& Request);
	FMCPResponse HandleMoveActorToFolder(const FMCPRequest& Request);
	FMCPResponse HandleAddActorTag(const FMCPRequest& Request);
	FMCPResponse HandleRemoveActorTag(const FMCPRequest& Request);

	// Spatial analysis methods
	FMCPResponse HandleMeasureDistance(const FMCPRequest& Request);
	FMCPResponse HandleFindActorsInRadius(const FMCPRequest& Request);
	FMCPResponse HandleFindActorsInBounds(const FMCPRequest& Request);
	FMCPResponse HandleRaycast(const FMCPRequest& Request);
	FMCPResponse HandleGetGroundHeight(const FMCPRequest& Request);

	// Pattern placement methods
	FMCPResponse HandlePlaceInGrid(const FMCPRequest& Request);
	FMCPResponse HandlePlaceAlongSpline(const FMCPRequest& Request);
	FMCPResponse HandlePlaceInCircle(const FMCPRequest& Request);
	FMCPResponse HandleScatterInArea(const FMCPRequest& Request);

	// Helper method for executing Python scripts
	FMCPResponse ExecutePythonFromParams(const FMCPRequest& Request);
};

