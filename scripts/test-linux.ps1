param(
    [switch]$SkipBuild,
    [ValidateRange(1, 64)][int]$Jobs = 4
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$imageName = 'aibox-tests:ubuntu24.04'

& docker info --format '{{.ServerVersion}}'
if ($LASTEXITCODE -ne 0) { throw 'Docker Linux engine is unavailable.' }

if (-not $SkipBuild) {
    & docker build --file (Join-Path $projectRoot 'tools/docker/Dockerfile.test') --tag $imageName (Join-Path $projectRoot 'tools/docker')
    if ($LASTEXITCODE -ne 0) { throw 'Failed to build the Linux test image.' }
}

# Build products and temporary test data stay in the disposable container.
$buildCommand = "cmake -S /workspace -B /tmp/aibox-build -DAIBOX_BUILD_TESTS=ON && cmake --build /tmp/aibox-build -j$Jobs && ctest --test-dir /tmp/aibox-build --output-on-failure"
& docker run --rm --mount "type=bind,source=$projectRoot,target=/workspace,readonly" $imageName bash -lc $buildCommand
if ($LASTEXITCODE -ne 0) { throw 'Linux build or tests failed; see output above.' }
