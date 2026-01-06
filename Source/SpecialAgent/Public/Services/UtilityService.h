// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/IMCPService.h"

/**
 * Utility Service
 * 
 * Editor utility operations like save, undo/redo, and selection management.
 * Methods: save_level, undo, redo, select_actor, get_selection
 */
class SPECIALAGENT_API FUtilityService : public IMCPService
{
public:
	FUtilityService();
	virtual ~FUtilityService() = default;

	// IMCPService interface
	virtual FMCPResponse HandleRequest(const FMCPRequest& Request, const FString& MethodName) override;
	virtual FString GetServiceDescription() const override;
	virtual TArray<FMCPToolInfo> GetAvailableTools() const override;

private:
	FMCPResponse HandleSaveLevel(const FMCPRequest& Request);
	FMCPResponse HandleUndo(const FMCPRequest& Request);
	FMCPResponse HandleRedo(const FMCPRequest& Request);
	FMCPResponse HandleSelectActor(const FMCPRequest& Request);
	FMCPResponse HandleGetSelection(const FMCPRequest& Request);
	FMCPResponse HandleGetSelectionBounds(const FMCPRequest& Request);
	FMCPResponse HandleSelectAtScreen(const FMCPRequest& Request);
};

