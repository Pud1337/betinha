# betinha

*betinha is a lightweight image, video and audio converter using ffmpeg, with an integrated youtube video downloader (yt-dlp).* 

### Supported Formats

| Images | Video | Audio | Youtube
| -------|:-----:| :---: | :---: |
| PNG    | GIF   | MP3   | MP3   |
| JPEG   | MP4   |       | MP4   |
| WEBP   |       |       | GIF   |

## Installation / Usage

### Dependencies 
- Python 3
- GTK 4
- FFmpeg
- yt-dlp ( the repo comes with a precompiled binary at | ./libs/yt-dlp| )

### Compiling:

```
$ git clone https://github.com/Pud1337/betinha.git 
$ cd betinha 
$ gcc converter-gtk4.c -o betinha $(pkg-config --cflags --libs gtk4) -lm -Wall
```
### Precompiled binary:

```
git clone https://github.com/Pud1337/betinha.git
```
- Open the *"betinha"* executable.

### Usage
- Open the executable.
- Select your input image/video/youtube link.
- Type the name desired for the converted file and/or use the | **browse...** | button to select a folder.
- Select the format you want it to be converted to.
- Convert!

## Images

![](https://raw.githubusercontent.com/Pud1337/randomstuff/refs/heads/main/betinha.png "The Betinha GUI")

# Heads-Up

> The yt-dlp path is hardcoded, so if you want to change the name/location for the binary you will have to recompile the executable. |
 The path for it is right at the start, on the following macro inclusion :
```
#define YTDLP_PATH  "./libs/yt-dlp"
```
