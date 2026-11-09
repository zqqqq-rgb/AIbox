param(
    [ValidateSet('setup','build','test','web-test','start','stop','status','shell','debug-services')]
    [string]$Action = 'start'
)
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$containerName = 'aibox-dev'
function DockerRun {
    # Bind the array explicitly: Windows PowerShell 5.1 can collapse an array
    # captured by ValueFromRemainingArguments into one native-command argument.
    param([Parameter(Mandatory=$true)][string[]]$DockerArguments)
    & docker @DockerArguments
    if ($LASTEXITCODE -ne 0) { throw "Docker command failed: $($DockerArguments[0])" }
}
if ($Action -eq 'setup') {
    & docker image inspect aibox-tests:ubuntu24.04 *> $null
    if ($LASTEXITCODE -ne 0) {
        DockerRun -DockerArguments @('build','-f',(Join-Path $projectRoot 'tools/docker/Dockerfile.test'),'-t','aibox-tests:ubuntu24.04',(Join-Path $projectRoot 'tools/docker'))
    }
    DockerRun -DockerArguments @('build','-f',(Join-Path $projectRoot '.devcontainer/Dockerfile'),'-t','aibox-dev:ubuntu24.04',$projectRoot)
}
& docker container inspect $containerName *> $null
if ($LASTEXITCODE -ne 0) {
    DockerRun -DockerArguments @('run','-d','--name',$containerName,
        '--mount',"type=bind,source=$projectRoot,target=/workspace",
        '--mount','type=volume,source=aibox-dev-build,target=/workspace/build-dev',
        '--mount','type=volume,source=aibox-dev-node-modules,target=/workspace/web/node_modules',
        '-p','127.0.0.1:8080:8080','-p','127.0.0.1:8081:8081','-p','127.0.0.1:8090:8090',
        'aibox-dev:ubuntu24.04')
} else {
    DockerRun -DockerArguments @('start',$containerName)
}
switch ($Action) {
    setup { DockerRun -DockerArguments @('exec',$containerName,'bash','tools/dev/setup.sh') }
    build { DockerRun -DockerArguments @('exec',$containerName,'cmake','--build','build-dev','-j4') }
    test { DockerRun -DockerArguments @('exec',$containerName,'ctest','--test-dir','build-dev','--output-on-failure') }
    web-test { DockerRun -DockerArguments @('exec',$containerName,'npm','test','--prefix','web') }
    start { DockerRun -DockerArguments @('exec',$containerName,'python3','tools/dev/demo.py','start') }
    debug-services { DockerRun -DockerArguments @('exec',$containerName,'python3','tools/dev/demo.py','start-services') }
    stop { DockerRun -DockerArguments @('exec',$containerName,'python3','tools/dev/demo.py','stop') }
    status { DockerRun -DockerArguments @('exec',$containerName,'curl','--noproxy','*','-fsS','http://127.0.0.1:8080/api/system/info') }
    shell { DockerRun -DockerArguments @('exec','-it',$containerName,'bash') }
}
