// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

class FSpecialAgentMCPServer;

/**
 * MCP Server Status for the status bar
 */
enum class EMCPServerStatus : uint8
{
	Offline,      // Server not running (Red)
	Listening,    // Server running, no clients (Orange)  
	Connected     // Server running with active client (Green)
};

/**
 * Status bar widget showing MCP server connection status
 */
class SMCPStatusBarWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMCPStatusBarWidget) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, TSharedPtr<FSpecialAgentMCPServer> InMCPServer);

private:
	/** Get the current status color */
	FSlateColor GetStatusColor() const;

	/** Get the tooltip text based on status */
	FText GetStatusTooltip() const;

	/** Get the current server status */
	EMCPServerStatus GetServerStatus() const;

	/** Handle click on the status widget */
	FReply OnStatusClicked();

	/** Timer callback to update status */
	EActiveTimerReturnType UpdateStatus(double InCurrentTime, float InDeltaTime);

	/** Reference to the MCP server */
	TWeakPtr<FSpecialAgentMCPServer> MCPServer;

	/** Cached status for display */
	EMCPServerStatus CachedStatus;

	/** Number of connected clients */
	int32 ConnectedClients;
};

