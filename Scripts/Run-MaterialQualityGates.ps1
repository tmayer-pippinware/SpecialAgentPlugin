param(
    [string]$ServerUrl = "http://localhost:8767",
    [int]$TimeoutSec = 20,
    [string]$RootPath = "/Game/SpecialAgentTests/MaterialQualityGates"
)

$ErrorActionPreference = "Stop"
$McpUrl = "$ServerUrl/mcp"
$RunStamp = Get-Date -Format "yyyyMMdd_HHmmss"
$Prefix = "MATQG_$RunStamp"

function Assert-True {
    param(
        [bool]$Condition,
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Invoke-MaterialTool {
    param(
        [string]$ToolName,
        [hashtable]$Arguments = @{},
        [switch]$AllowFailure
    )

    $requestId = "mat_qg_$([Guid]::NewGuid().ToString('N'))"
    $requestBody = @{
        jsonrpc = "2.0"
        id = $requestId
        method = "material/$ToolName"
        params = $Arguments
    }

    $jsonBody = $requestBody | ConvertTo-Json -Depth 32 -Compress
    $response = Invoke-RestMethod -Uri $McpUrl -Method Post -ContentType "application/json" -Body $jsonBody -TimeoutSec $TimeoutSec

    if ($null -ne $response.error) {
        throw "RPC error for material/${ToolName}: $($response.error.message)"
    }
    if ($null -eq $response.result) {
        throw "No result payload for material/${ToolName}"
    }

    $result = $response.result
    if (-not $AllowFailure -and $result.success -ne $true) {
        throw "Tool material/${ToolName} failed: $($result.error)"
    }
    return $result
}

Write-Host "Checking server health at $ServerUrl/health..."
$health = Invoke-RestMethod -Uri "$ServerUrl/health" -Method Get -TimeoutSec $TimeoutSec
Assert-True ($health.status -eq "healthy") "Health check failed: expected status=healthy"

$state = [ordered]@{
    MaterialPath = "$RootPath/${Prefix}_Master"
    MaterialInstancePath = "$RootPath/${Prefix}_MI"
    MaterialFunctionAPath = "$RootPath/${Prefix}_FuncA"
    MaterialFunctionBPath = "$RootPath/${Prefix}_FuncB"
    ParameterCollectionPath = "$RootPath/${Prefix}_MPC"
    ScalarParameterName = "GateScalar"
    VectorParameterName = "GateBaseColor"
}

$tests = @()
$tests += @{
    Name = "Smoke: create material, add parameters, wire outputs, compile"
    Run = {
        Invoke-MaterialTool -ToolName "create_material" -Arguments @{
            material_path = $state.MaterialPath
        } | Out-Null

        Invoke-MaterialTool -ToolName "add_parameter" -Arguments @{
            asset_path = $state.MaterialPath
            parameter_type = "scalar"
            parameter_name = $state.ScalarParameterName
            default_value = 0.35
            node_pos_x = -420
            node_pos_y = 80
        } | Out-Null

        $vectorParam = Invoke-MaterialTool -ToolName "add_parameter" -Arguments @{
            asset_path = $state.MaterialPath
            parameter_type = "vector"
            parameter_name = $state.VectorParameterName
            default_value = @{
                r = 0.2
                g = 0.35
                b = 0.85
                a = 1.0
            }
            node_pos_x = -420
            node_pos_y = -120
        }

        Assert-True ($null -ne $vectorParam.parameter.node_id) "Vector parameter creation did not return node_id"

        $scalarList = Invoke-MaterialTool -ToolName "list_parameters" -Arguments @{
            asset_path = $state.MaterialPath
            parameter_type = "scalar"
        }
        $scalarNode = $scalarList.parameters | Where-Object { $_.parameter_name -eq $state.ScalarParameterName } | Select-Object -First 1
        Assert-True ($null -ne $scalarNode) "Could not resolve scalar parameter node"

        Invoke-MaterialTool -ToolName "set_material_output" -Arguments @{
            asset_path = $state.MaterialPath
            output_name = "roughness"
            from_node_id = $scalarNode.node_id
        } | Out-Null

        Invoke-MaterialTool -ToolName "set_material_output" -Arguments @{
            asset_path = $state.MaterialPath
            output_name = "base_color"
            from_node_id = $vectorParam.parameter.node_id
        } | Out-Null

        $compile = Invoke-MaterialTool -ToolName "compile_material" -Arguments @{
            asset_path = $state.MaterialPath
            include_messages = $true
        }
        Assert-True ($compile.success -eq $true) "compile_material did not report success"
    }
}

$tests += @{
    Name = "Regression: parameter metadata round-trip"
    Run = {
        Invoke-MaterialTool -ToolName "set_parameter_metadata" -Arguments @{
            asset_path = $state.MaterialPath
            parameter_name = $state.ScalarParameterName
            group = "QualityGates"
            sort_priority = 42
            description = "Material quality gate scalar"
        } | Out-Null

        $params = Invoke-MaterialTool -ToolName "list_parameters" -Arguments @{
            asset_path = $state.MaterialPath
        }
        $scalar = $params.parameters | Where-Object { $_.parameter_name -eq $state.ScalarParameterName } | Select-Object -First 1
        Assert-True ($null -ne $scalar) "Scalar parameter missing after metadata set"
        Assert-True ($scalar.group -eq "QualityGates") "Group round-trip mismatch"
        Assert-True ([int]$scalar.sort_priority -eq 42) "Sort priority round-trip mismatch"
        Assert-True ($scalar.description -eq "Material quality gate scalar") "Description round-trip mismatch"
    }
}

$tests += @{
    Name = "Regression: material instance override round-trip"
    Run = {
        Invoke-MaterialTool -ToolName "create_material_instance" -Arguments @{
            material_instance_path = $state.MaterialInstancePath
            parent_material_path = $state.MaterialPath
        } | Out-Null

        Invoke-MaterialTool -ToolName "material_instance/set_scalar" -Arguments @{
            material_instance_path = $state.MaterialInstancePath
            parameter_name = $state.ScalarParameterName
            value = 0.77
        } | Out-Null

        $overrides = Invoke-MaterialTool -ToolName "material_instance/list_overrides" -Arguments @{
            material_instance_path = $state.MaterialInstancePath
        }
        Assert-True ([int]$overrides.scalar_override_count -ge 1) "Expected at least one scalar override"
        $scalarOverride = $overrides.scalar_overrides | Where-Object { $_.parameter_info.name -eq $state.ScalarParameterName } | Select-Object -First 1
        Assert-True ($null -ne $scalarOverride) "Scalar override for GateScalar was not found"
    }
}

$tests += @{
    Name = "Regression: material function create/edit/compile"
    Run = {
        Invoke-MaterialTool -ToolName "create_material_function" -Arguments @{
            material_function_path = $state.MaterialFunctionAPath
        } | Out-Null

        $funcInput = Invoke-MaterialTool -ToolName "material_function/create_input" -Arguments @{
            material_function_path = $state.MaterialFunctionAPath
            input_name = "InValue"
            input_type = "float3"
            node_pos_x = -300
            node_pos_y = 0
        }

        Invoke-MaterialTool -ToolName "material_function/create_output" -Arguments @{
            material_function_path = $state.MaterialFunctionAPath
            output_name = "Result"
            from_node_id = $funcInput.input.node_id
            from_output_index = 0
            node_pos_x = 120
            node_pos_y = 0
        } | Out-Null

        $compile = Invoke-MaterialTool -ToolName "material_function/compile" -Arguments @{
            material_function_path = $state.MaterialFunctionAPath
        }
        Assert-True ($compile.success -eq $true) "material_function/compile did not report success"
    }
}

$tests += @{
    Name = "Regression: parameter collection create/edit round-trip"
    Run = {
        Invoke-MaterialTool -ToolName "create_parameter_collection" -Arguments @{
            parameter_collection_path = $state.ParameterCollectionPath
        } | Out-Null

        Invoke-MaterialTool -ToolName "material_collection/add_scalar" -Arguments @{
            parameter_collection_path = $state.ParameterCollectionPath
            parameter_name = "CollectionScalar"
            default_value = 1.25
        } | Out-Null

        Invoke-MaterialTool -ToolName "material_collection/rename_parameter" -Arguments @{
            parameter_collection_path = $state.ParameterCollectionPath
            old_parameter_name = "CollectionScalar"
            new_parameter_name = "CollectionScalarRenamed"
            parameter_type = "scalar"
        } | Out-Null

        Invoke-MaterialTool -ToolName "material_collection/set_default_value" -Arguments @{
            parameter_collection_path = $state.ParameterCollectionPath
            parameter_name = "CollectionScalarRenamed"
            parameter_type = "scalar"
            value = 2.5
        } | Out-Null

        $collectionParams = Invoke-MaterialTool -ToolName "material_collection/list_parameters" -Arguments @{
            parameter_collection_path = $state.ParameterCollectionPath
        }
        $scalarParam = $collectionParams.parameters | Where-Object { $_.parameter_name -eq "CollectionScalarRenamed" } | Select-Object -First 1
        Assert-True ($null -ne $scalarParam) "Renamed collection scalar was not found"
    }
}

$tests += @{
    Name = "Regression: rename/refactor safety"
    Run = {
        Invoke-MaterialTool -ToolName "create_material_function" -Arguments @{
            material_function_path = $state.MaterialFunctionBPath
        } | Out-Null

        $callNode = Invoke-MaterialTool -ToolName "material_function/add_call_node" -Arguments @{
            asset_path = $state.MaterialPath
            material_function_path = $state.MaterialFunctionAPath
            node_pos_x = -120
            node_pos_y = 280
        }
        Assert-True ($null -ne $callNode.node.node_id) "Function call node creation failed"

        $replace = Invoke-MaterialTool -ToolName "replace_function_calls" -Arguments @{
            asset_path = $state.MaterialPath
            old_function_path = $state.MaterialFunctionAPath
            new_function_path = $state.MaterialFunctionBPath
        }
        Assert-True ($replace.changed_count -ge 1) "Expected at least one replaced function call"

        Invoke-MaterialTool -ToolName "rename_symbol" -Arguments @{
            asset_path = $state.MaterialPath
            symbol_type = "parameter"
            old_name = $state.ScalarParameterName
            new_name = "GateScalarRenamed"
        } | Out-Null

        $refs = Invoke-MaterialTool -ToolName "find_references" -Arguments @{
            asset_path = $state.MaterialPath
            symbol_type = "parameter"
            symbol_name = "GateScalarRenamed"
        }
        Assert-True ($refs.target_count -ge 1) "Renamed parameter reference target was not found"
        $state.ScalarParameterName = "GateScalarRenamed"
    }
}

$tests += @{
    Name = "Contract: error payload consistency"
    Run = {
        $missing = Invoke-MaterialTool -ToolName "get_material_info" -Arguments @{
            asset_path = "/Game/DoesNotExist/DefinitelyMissingMaterial"
        } -AllowFailure

        Assert-True ($missing.success -eq $false) "Missing-asset call should return success=false"
        Assert-True (-not [string]::IsNullOrWhiteSpace($missing.error)) "Missing-asset call should return error text"
    }
}

$passed = 0
$failed = 0
$failures = @()

Write-Host "Running material quality gates..."
foreach ($test in $tests) {
    try {
        & $test.Run
        $passed++
        Write-Host "[PASS] $($test.Name)"
    }
    catch {
        $failed++
        $failures += "[FAIL] $($test.Name): $($_.Exception.Message)"
        Write-Host "[FAIL] $($test.Name)"
    }
}

Write-Host ""
Write-Host "Material quality gates complete."
Write-Host "Passed: $passed"
Write-Host "Failed: $failed"
Write-Host "Generated assets root: $RootPath"
Write-Host "Run prefix: $Prefix"
Write-Host "Assets are left in project for inspection."

if ($failed -gt 0) {
    Write-Host ""
    Write-Host "Failures:"
    $failures | ForEach-Object { Write-Host $_ }
    exit 1
}

exit 0
