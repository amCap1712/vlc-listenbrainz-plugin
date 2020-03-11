# vlc-listenbrainz-plugin
A plugin to submit listens from VLC to Listenbrainz.

VLC supports two ways to distribute plugins: in tree and out of tree. In tree plugins come bundled with the VLC 
installation. Out of tree plugins have to manually installed after VLC has been installed.
As of now, this plugin is not included with the official VLC distributions. Once this plugin has been tested thoroughly,
 it will be submitted to the official VLC repo. Till then, you can continue to test and try out the plugin by compiling 
 VLC yourself. 
 
#### Compiling out of tree
##### Linux
1. Follow [these instructions](https://wiki.videolan.org/OutOfTreeCompile/) to install the VLC development files.
2. Clone this git repository.
3. `cd` into the repository. Run `sudo make install`.

The plugin shared library file will be generated and copied to the VLC plugins folder. 
##### Windows
There are two ways to compile the plugin for windows: 
- cross-compiling for Windows on a linux machine. The steps are similar to cross compiling VLC. For details, see 
[this](https://forum.videolan.org/viewtopic.php?t=146175).
- using Cygwin, MSYS2, MINGW or any other toolchain on windows. The steps should be more or less the same irrespective 
of the toolchain. 

Here are the instructions to build using MSYS2 on Windows:
1. Install the mingw64 toolchain and other required packages. 
    ```shell script
    pacman -S base-devel mingw-w64-x86_64-toolchain pkg-config make
    ```
2. Obtain the libVLC SDK by downloading .7z or .zip VLC archive. Extract the archive. Open MINGW shell from MSYS directory.
 `cd` into the extracted vlc archive. Then, execute
    ```shell script
    cd sdk
    sed -i "s|^prefix=.*|prefix=${PWD}|g" lib/pkgconfig/*.pc
    export PKG_CONFIG_PATH="${PWD}/lib/pkgconfig:$PKG_CONFIG_PATH"
    ```
3. Clone this git repository.
4. `cd` into the repository. Run `make`.
The __liblistenbrainz_plugin.dll__ file will be generated.

##### macOS
1. Install development toolchain. Launch the Terminal and type the following command:
    ```shell script
    brew install make gcc pkg-config
    ```
2. VLC for macOS contains all libVLC SDK files except some header and pkg-config files. The missing files from libVLC SDK 
for Windows.
    ```shell script
    curl -OL ftp://ftp.videolan.org/pub/videolan/vlc/3.0.8/win64/vlc-3.0.8-win64.7z
    brew install p7zip
    7za x vlc-3.0.8-win64.7z -owin-sdk sdk\* -r
    
    mkdir macos-sdk && cd macos-sdk
    cp -r /Applications/VLC.app/Contents/MacOS/{include,lib} .
    cp -r ../win-sdk/vlc-3.0.8/sdk/include/vlc/plugins ./include/vlc/
    cp -r ../win-sdk/vlc-3.0.8/sdk/lib/pkgconfig ./lib/
    sed -i "" "s|^prefix=.*|prefix=${PWD}|g" lib/pkgconfig/*.pc
    export PKG_CONFIG_PATH="${PWD}/lib/pkgconfig"
    ```
3. Clone this git repository.
4. `cd` into the repository. Run `make`. The __liblistenbrainz_plugin.dylib__ file will be generated.

#### Compiling in tree
Here are the instructions to build the plugin as an in tree plugin:
1. Clone the VLC git repo.
2. Copy the `vlc-3.0\listenbrainz.c` file from this repository to `<your local vlc repo path>\modules\misc` directory.
3. Add the following lines to `Makefile.am` in `<your local vlc repo path>\modules\misc`.
    ```
    liblistenbrainz_plugin_la_SOURCES = misc/listenbrainz.c
    liblistenbrainz_plugin_la_LIBADD = $(SOCKET_LIBS)
    misc_LTLIBRARIES += liblistenbrainz_plugin.la
    ```
4. Build VLC.

### Using the plugin
To use the plugin, you can either compile it manually by following the steps above or you can download your OS specific 
plugin file.

Linux: [liblistenbrainz_plugin.so](https://github.com/amCap1712/vlc-listenbrainz-plugin/releases/download/v1.0/liblistenbrainz_plugin.so)

Windows: [liblistenbrainz_plugin.dll](https://github.com/amCap1712/vlc-listenbrainz-plugin/releases/download/v1.0/liblistenbrainz_plugin.dll)

macOS: [liblistenbrainz_plugin.dylib](https://github.com/amCap1712/vlc-listenbrainz-plugin/releases/download/v1.0/liblistenbrainz_plugin.dylib)
1. - **Linux**: If you compiled the plugin yourself, you can skip this step. Copy the file to __<vlc-plugin-directory>\misc__.
 To find the plugin directory, execute `pkg-config --variable=pluginsdir vlc-plugin` in the terminal.
   - **macOS**: Go to __Finder->Applications__ and right click on __VLC__. 
        - Click on __Show Package Contents__ and browse the 
   __Contents\MacOS\plugins__ folder (`/Applications/VLC.app/Contents/MacOS/plugins`)
        - Copy the __liblistenbrainz_plugin.dylib__ to `plugins` folder

   
   - **Windows**: Copy the __liblistenbrainz_plugin.dll__ to __<vlc-installation-directory>\plugins\misc__.
2. Run VLC. 
    - Go to `Preferences`, press `Show All` button in the left right corner.
    - In the __Preferences->Interfaces->Control Interfaces__ section, enable the listenbrainz plugin. 
        _(If the plugin does not show up in the list, you might need to clear the plugins cache or reset your preferences)_
3. Enter your ListenBrainz User Token in the required field. This token can be found in the [Profile section](https://listenbrainz.org/profile/) of your profile.

You are all set to submit listens from VLC to ListenBrainz.