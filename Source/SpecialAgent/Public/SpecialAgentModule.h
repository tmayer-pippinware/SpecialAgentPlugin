// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FSpecialAgentMCPServer;
class SWidget;
class FExtender;

/**
 * SpecialAgent Plugin Module
 * 
 * Main module for the SpecialAgent MCP Server plugin.
 * Manages the lifecycle of the MCP server and provides access to services.
 */
class FSpecialAgentModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/**
	 * Get the singleton instance of the MCP server
	 */
	TSharedPtr<FSpecialAgentMCPServer> GetMCPServer() const { return MCPServer; }

	/**
	 * Check if the MCP server is running
	 */
	bool IsMCPServerRunning() const;

private:
	/** Register the status bar widget */
	void RegisterStatusBarWidget();

	/** Unregister the status bar widget */
	void UnregisterStatusBarWidget();

	/** The MCP server instance */
	TSharedPtr<FSpecialAgentMCPServer> MCPServer;

	/** Extender for the toolbar */
	TSharedPtr<FExtender> ToolBarExtender;
};
