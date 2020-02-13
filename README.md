# vlc-listenbrainz-plugin
A plugin to submit listens from VLC to Listenbrainz.

VLC supports two ways to distribute plugins: in tree and out of tree. In tree plugins come bundled with the VLC 
installation. Out of tree plugins have to manually installed after VLC has been installed.
As of now, this plugin is not included with the official VLC distributions. Once this plugin has been tested thoroughly,
 it will be submitted to the official VLC repo. Till then, you can continue to test and try out the plugin by compiling 
 VLC yourself. 
 
#### Compiling out of tree
Here are the instructions to build the plugin file on a linux distribution:
1. Follow The instructions at https://wiki.videolan.org/OutOfTreeCompile/ to install the VLC development files.
2. Clone this git repository.
3. Execute `cd <path-to-repository\vlc-3.0`
4. Run `sudo make install`.
The plugin shared library file will be generated and copied to the VLC plugins folder. 

#### Compiling in tree
macOS has no OS-provided package manager. Thus, the plugin can only be compiled in tree. Here are the instructions to 
build it as an in tree plugin:
1. Clone the VLC git repo.
2. Copy the `vlc-3.0\listenbrainz.c` file from this repository to `<your local vlc repo path>\modules\misc` directory.
3. Add the following lines to `Makefile.am` in `<your local vlc repo path>\modules\misc`.
    ```
    liblistenbrainz_plugin_la_SOURCES = misc/listenbrainz.c
    liblistenbrainz_plugin_la_LIBADD = $(SOCKET_LIBS)
    misc_LTLIBRARIES += liblistenbrainz_plugin.la
    ```
4. Build VLC.
5. Run VLC and enable the listenbrainz plugin in `Preferences->Interfaces->Control Interfaces`.
6. Enter your ListenBrainz User Token in the required field. This token can be found in the Profile section of your 
ListenBrainz Account on the ListenBrainz website.

You are all set to submit listens from VLC to ListenBrainz.
