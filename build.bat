setlocal

set os=windows
set arch=x86_64
set file=onnxruntime.lib
set version=v1.14.1-neuralnote.1
set dir=onnxruntime-%version%-%os%-%arch%
set archive=%dir%.tar.gz
:: SHA-256 of the v1.14.1-neuralnote.1 tarball; keep in step with ONNX_SHA256 in run.py.
set sha256=0fe0568469956181fafffdfb416916c10919114b0c8ce2badddd0a9a313ebab8

:: logical OR is missing in batch o_O!
@set extract=false
@if not exist "Lib\ModelData\features_model.ort" set extract=true
@if not exist "ThirdParty\onnxruntime\lib\%file%" set extract=true
@if exist "%archive%" set extract=true

if "%extract%" == "true" (
	if not exist "%archive%" (
		curl -fSLO "https://github.com/tiborvass/libonnxruntime-neuralnote/releases/download/%version%/%archive%" || exit /b
	)
	call :verify "%archive%" || exit /b 1
	del /s /f /q ThirdParty\%dir% ThirdParty\onnxruntime
	rmdir /s /q ThirdParty\%dir% ThirdParty\onnxruntime
	tar -C ThirdParty -x -v -f %archive%
	rename ThirdParty\%dir% onnxruntime
	move ThirdParty\onnxruntime\model.with_runtime_opt.ort Lib\ModelData\features_model.ort
	del %archive%
)

wmic cpu get NumberOfLogicalProcessors | findstr /V NumberOfLogicalProcessors > ncpus
set /p ncpus=<ncpus
del ncpus

set config=Release
cmake -S . -B build -DBUILD_UNIT_TESTS=ON || exit /b
cmake --build build -j %ncpus% --config %config% --target Quarry_Standalone --target UnitTests --target Quarry_VST3 --target Quarry_AU

.\build\Tests\UnitTests_artefacts\Release\UnitTests.exe || exit /b

echo
echo "Run .\build\Quarry_artefacts\Release\Standalone\Quarry.exe"
exit /b 0

:: certutil prints "SHA256 hash of <file>:", then the hash, then a "completed successfully" line,
:: so the hash is the first line after the header. An archive that does not match is deleted so
:: the next run downloads it again rather than trusting it.
:verify
set "got="
for /f "skip=1 delims=" %%h in ('certutil -hashfile "%~1" SHA256') do if not defined got set "got=%%h"
if not defined got (
	echo Could not compute the SHA-256 of %~1
	exit /b 1
)
set "got=%got: =%"
if /i not "%got%"=="%sha256%" (
	echo The onnxruntime archive is not the expected file:
	echo   expected sha256 %sha256%
	echo   got             %got%
	del /f /q "%~1"
	exit /b 1
)
exit /b 0
