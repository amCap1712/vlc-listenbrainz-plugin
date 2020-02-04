# vlc-listenbrainz-plugin
A plugin to submit listens from VLC to Listenbrainz.

As of now, this plugin is not included with the official VLC distributions. Once this plugin has been tested thoroughly, it will be submitted to the official VLC repo. Till then, you can continue to test and try out the plugin by compiling VLC yourself. 

Follow these steps to use the plugin:
1. Clone the VLC git repo.
2. Copy the `listenbrainz.c` file from this repository to `<your local vlc repo path>\modules\misc` directory.
3. Add the following lines to `Makefile.am` in `<your local vlc repo path>\modules\misc`.
    ```
    liblistenbrainz_plugin_la_SOURCES = misc/listenbrainz.c
    liblistenbrainz_plugin_la_LIBADD = $(SOCKET_LIBS)
    misc_LTLIBRARIES += liblistenbrainz_plugin.la
    ```
4. Build VLC.
5. Run VLC and enable the listenbrainz plugin in `Preferences->Interfaces->Control Interfaces`.
6. Enter your ListenBrainz User Token in the required field. This token can be found in the Profile section of your ListenBrainz Account on the ListenBrainz website.

You are all set to submit listens from VLC to ListenBrainz.
