# MKVSource
Media Foundation Source for MKV (Matroska)

This is a very messy and hacked together Media Foundation Source for Matroska.  It ONLY works on H264 video and AC3 audio
streams within an MKV file.  Other stream-type decoders can be added very easily, provided you understand how to translate
the way those streams are encoded within a matroska container into the original stream.  

As for subtitles, I was just starting to explore how to do that but my parser should be picking out those parts of the container.
It's just not doing anything with them.

Let me know if you would like to contribute!  Unless there is interest, I'm probably not going to do much with this at the moment.

Lee McPherson
8/26/2015
