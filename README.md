# VideoSlideShow

A Windows media slideshow application built with **Direct3D 11**, the **Windows Imaging Component (WIC)**, and **Windows Media Foundation**.

## Features

- Reads a mixed list of images and videos from `playlist.txt` (one file path per line).
- Renders two full-screen *planes* using Direct3D 11:
  - **Plane A** – currently visible content (image or playing video).
  - **Plane B** – the next item, positioned off-screen to the right.
- **Left-click anywhere** to trigger a transition:
  - The current video on Plane A is paused.
  - The next media item is loaded onto Plane B.
  - Plane A slides out to the left while Plane B slides in from the right (smooth ease-in/out animation, 0.6 s).
- After the last item the playlist loops back to the first.
- Press **Escape** to quit.

## Supported formats

| Category | Extensions |
|----------|-----------|
| Images   | JPEG, PNG, BMP, TIFF, GIF, and any WIC-decodable format |
| Videos   | MP4, WMV, AVI, MOV, MKV, M4V, MPEG, WebM, and any format supported by Windows Media Foundation on the host machine |

## Building

Open `VideoSlideShow.sln` in **Visual Studio 2022** and build the `x64 Release` (or `x64 Debug`) configuration.

Requirements:
- Windows 10 SDK (10.0 or later)
- MSVC v143 toolset or newer

## Running

1. Place `playlist.txt` next to the compiled `VideoSlideShow.exe`.
2. Edit `playlist.txt` – add one image or video file path per line (absolute or relative to the `.exe` directory). Lines beginning with `#` are treated as comments and skipped.
3. Run `VideoSlideShow.exe`.

Example `playlist.txt`:
```
images\photo1.jpg
images\photo2.png
videos\clip1.mp4
videos\clip2.wmv
```

## Architecture

```
WinMain
 └─ Application::Initialize()
     ├─ MFStartup / CoCreateInstance(WICImagingFactory)
     ├─ D3D11 device + swap chain
     ├─ Compile HLSL vertex & pixel shaders (embedded as string literals)
     ├─ Full-screen quad geometry buffers
     ├─ Per-plane constant buffers (translation matrix)
     └─ LoadMedia(plane 0, index 0)   ← first item on screen

Message loop  →  Application::Update() + Application::Render()

WM_LBUTTONDOWN  →  Application::OnClick()
    ├─ Pause video on visible plane
    ├─ LoadMedia(standby plane, next index)
    │    ├─ Image: WIC decoder → BGRA pixel buffer → D3D11 texture (main thread)
    │    └─ Video: IMFSourceReader on a background thread → BGRA frames
    │               → shared pixel buffer → UpdateSubresource each frame
    └─ Start slide animation (ease-in/out, 0.6 s)
```
