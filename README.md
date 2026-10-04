# Tartarus Engine

A custom C++ / OpenGL 3D game engine (GLFW, EnTT, PhysX, Dear ImGui) with its own editor.

## Assets

Third-party models, textures and sounds are not in git. On a new clone, copy them in from the shared
"Tartarus Assets" Google Drive folder:

    powershell -ExecutionPolicy Bypass -File tools\assets\fetch-assets.ps1 -Source "<path to Tartarus Assets>"

Without them the editor still runs, but characters, weapons and sounds are missing.

## Build and run

    cmake --build build --config Release --target TartarusEngine

`run-editor.cmd` rebuilds the checkout and launches the editor.

## Tests

    build\Release\TartarusEngine.exe --unit-tests
    build\Release\TartarusEngine.exe --smoke-test tests\smoke-scenes
