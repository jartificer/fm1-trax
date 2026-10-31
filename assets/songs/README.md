# Songs

- **SunsetBeach.vgm**: "Sunset Beach Galaxy" by Tobikomi, from the YM2017 music cart
  (https://github.com/andwn/ym2017, `res/bgm/SunsetBeach.vgm` at commit 2bcf5dc), released there
  under the GNU General Public License v3.0, like FM1-TRAX. It is the demo song every FM-1 starts
  with and the web editor's Demo button loads, converted by `tools/vgm2fm1d.mjs` into
  `firmware/src/trk/demo_song_data.h`:

      node tools/vgm2fm1d.mjs assets/songs/SunsetBeach.vgm --header firmware/src/trk/demo_song_data.h \
          --dac 0="KICK TIGHT" --dac 1=SNARE --dac 2="TOM HIGH" --dac 3="TOM MID" --dac 4="TOM LOW" \
          --dac 5="HAT CLOSED" --dac 6=CRASH

  The song's own drum samples are played by FM1-TRAX's kit drums named there (it has no room
  for a song's samples).
