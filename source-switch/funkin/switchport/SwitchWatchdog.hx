package funkin.switchport;

import flixel.FlxG;

/** Feeds the native watchdog (lime-nx Main.cpp) with the current music/vocals state. Diagnostics only. */
@:cppFileCode('extern "C" volatile int cne_music_ms; extern "C" volatile int cne_vocals_ms; extern "C" volatile int cne_audio_flags; extern "C" volatile int cne_volumes;')
class SwitchWatchdog {
	public static function update() {
		var music = FlxG.sound.music;
		var ps = funkin.game.PlayState.instance;
		var musicMs:Int = music != null && music.playing ? Std.int(music.time) : -1;
		var vocalsMs:Int = ps != null && ps.vocals != null && ps.vocals.playing ? Std.int(ps.vocals.time) : -1;
		var flags:Int = (ps != null ? 1 : 0) | (music != null && music.playing ? 2 : 0) | (ps != null && ps.paused ? 4 : 0) | (FlxG.state != null && FlxG.state.subState != null ? 8 : 0);
		untyped __cpp__("cne_music_ms = {0}; cne_vocals_ms = {1}; cne_audio_flags = {2};", musicMs, vocalsMs, flags);
		var master = Std.int(FlxG.sound.volume * 100) & 255;
		var musicVol = music != null ? Std.int(music.volume * 100) & 255 : 255;
		var vocalsVol = ps != null && ps.vocals != null ? Std.int(ps.vocals.volume * 100) & 255 : 255;
		var packed = master | (musicVol << 8) | (vocalsVol << 16) | ((FlxG.sound.muted ? 1 : 0) << 24);
		untyped __cpp__("cne_volumes = {0};", packed);
	}
}
