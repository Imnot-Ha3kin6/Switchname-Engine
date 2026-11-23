package hxvlc.flixel;

import flixel.FlxG;
import flixel.FlxSprite;
import flixel.graphics.FlxGraphic;
import flixel.sound.FlxSound;
import flixel.util.FlxSignal;
import haxe.io.Bytes;
import lime.graphics.Image;
import lime.graphics.ImageBuffer;
import lime.media.AudioBuffer;
import lime.utils.UInt8Array;
import openfl.display.BitmapData;
import openfl.media.Sound;

/**
 * Switch implementation of hxvlc's `FlxVideoSprite`.
 *
 * hxvlc needs libVLC, which is not available on Nintendo Switch. This class keeps the
 * same API that mods use (`load`, `play`, `pause`, `resume`, `stop`, `bitmap.time`,
 * `bitmap.onEndReached`, ...) but decodes with FFmpeg (source-switch/native/cne_video.cpp):
 * video frames on a background thread, audio decoded up front and played as a normal sound.
 * The video follows the audio clock so it stays in sync with what you hear.
 */
@:keep
@:buildXml('<files id="haxe"><compilerflag value="-I${DEVKITPRO}/portlibs/switch/include"/></files>')
@:cppFileCode('#include "../../../../../../../source-switch/native/cne_video.cpp"')
class FlxVideoSprite extends FlxSprite {
	public var bitmap(default, null):FlxVideoBitmap;
	public var autoPause:Bool = true;
	public var autoVolumeHandle:Bool = true;

	@:noCompletion var __handle:cpp.RawPointer<cpp.Void> = null;
	@:noCompletion var __open:Bool = false;
	@:noCompletion var __frameBytes:Bytes;
	@:noCompletion var __image:Image;
	@:noCompletion var __sound:FlxSound;
	@:noCompletion var __duration:Float = 0;
	@:noCompletion var __clock:Float = 0;
	@:noCompletion var __loop:Bool = false;
	@:noCompletion var __playing:Bool = false;
	@:noCompletion var __paused:Bool = false;
	@:noCompletion var __held:Bool = false;
	@:noCompletion var __everShown:Bool = false;
	@:noCompletion var __dbgTimer:Float = 0;
	@:noCompletion var __dbgWall:Float = 0;
	@:noCompletion var __audio:AudioBuffer;
	@:noCompletion var __srcScaleX:Float = 1;
	@:noCompletion var __srcScaleY:Float = 1;

	override function drawFrameComplex(frame:flixel.graphics.frames.FlxFrame, camera:flixel.FlxCamera):Void {
		if (__srcScaleX == 1 && __srcScaleY == 1) {
			super.drawFrameComplex(frame, camera);
			return;
		}
		// same as FlxSprite, with the smaller texture stretched to the original video size first
		frame.prepareMatrix(_matrix, flixel.graphics.frames.FlxFrame.FlxFrameAngle.ANGLE_0, checkFlipX(), checkFlipY());
		_matrix.scale(__srcScaleX, __srcScaleY);
		_matrix.translate(-origin.x, -origin.y);
		_matrix.translate(-frameOffset.x, -frameOffset.y);
		_matrix.scale(scale.x, scale.y);
		if (bakedRotationAngle <= 0) {
			updateTrig();
			if (angle != 0) _matrix.rotateWithTrig(_cosAngle, _sinAngle);
		}
		getScreenPosition(_point, camera).subtractPoint(offset).addPoint(origin);
		_matrix.translate(_point.x, _point.y);
		if (isPixelPerfectRender(camera)) {
			_matrix.tx = Math.floor(_matrix.tx);
			_matrix.ty = Math.floor(_matrix.ty);
		}
		doAdditionalMatrixStuff(_matrix, camera);
		if (layer != null)
			layer.drawPixels(this, camera, frame, framePixels, _matrix, colorTransform, blend, antialiasing, shaderEnabled ? shader : null, wrapMode);
		else
			camera.drawPixels(frame, framePixels, _matrix, colorTransform, blend, antialiasing, shaderEnabled ? shader : null, wrapMode);
	}

	public function new(x:Float = 0, y:Float = 0) {
		super(x, y);
		bitmap = new FlxVideoBitmap(this);
		makeGraphic(1, 1, 0x00000000);
	}

	public function load(location:Dynamic, ?options:Array<String>):Bool {
		__close();
		var path:String = Std.string(location);
		// mods often pass asset IDs ("assets/videos/x.mp4"); hxvlc resolves those through the asset system
		if (!sys.FileSystem.exists(path) && openfl.utils.Assets.exists(path)) {
			var resolved = openfl.utils.Assets.getPath(path);
			if (resolved != null) path = resolved;
		}
		__loop = options != null && Lambda.exists(options, o -> StringTools.startsWith(o, "input-repeat"));

		__handle = untyped __cpp__("cnev_open({0}.utf8_str())", path);
		__open = untyped __cpp__("{0} != nullptr", __handle);
		if (!__open) {
			var reason:String = untyped __cpp__("::String(cnev_last_error())");
			funkin.backend.system.Logs.warn('Could not open video: $path ($reason)');
			return false;
		}

		var w:Int = untyped __cpp__("cnev_width({0})", __handle);
		var h:Int = untyped __cpp__("cnev_height({0})", __handle);
		__duration = untyped __cpp__("cnev_duration({0})", __handle);

		// frame buffer shared between the decoder (writes) and the texture (reads)
		__frameBytes = Bytes.alloc(w * h * 4);
		var buffer = new ImageBuffer(UInt8Array.fromBytes(__frameBytes), w, h, 32, BGRA32); // OpenFL textures are BGRA on Switch
		buffer.premultiplied = true; // opaque video: no per-frame premultiply copy
		__image = new Image(buffer);
		var bmp = BitmapData.fromImage(__image, false);
		loadGraphic(FlxGraphic.fromBitmapData(bmp, false, null, false));

		// Videos re-encoded smaller for Switch carry their original size; behave as that size
		// (frame size, hitbox, origin) so scripts that position/scale them for the original still line up.
		var srcW:Int = untyped __cpp__("cnev_source_size({0}, 0)", __handle);
		var srcH:Int = untyped __cpp__("cnev_source_size({0}, 1)", __handle);
		if (srcW > 0 && srcH > 0 && (srcW != w || srcH != h)) {
			__srcScaleX = srcW / w;
			__srcScaleY = srcH / h;
			@:privateAccess { frameWidth = srcW; frameHeight = srcH; }
			width = srcW * Math.abs(scale.x);
			height = srcH * Math.abs(scale.y);
			centerOrigin();
		} else {
			__srcScaleX = __srcScaleY = 1;
		}

		// audio
		var bytes:Int = untyped __cpp__("cnev_audio_bytes({0})", __handle);
		if (bytes > 0) {
			var pcm = Bytes.alloc(bytes);
			untyped __cpp__("cnev_audio_copy({0}, (unsigned char*){1}->GetBase())", __handle, pcm.getData());
			var ab = new AudioBuffer();
			ab.bitsPerSample = 16;
			ab.channels = untyped __cpp__("cnev_audio_channels({0})", __handle);
			ab.sampleRate = untyped __cpp__("cnev_audio_rate({0})", __handle);
			ab.data = UInt8Array.fromBytes(pcm);
			__audio = ab; // the sound itself is created on the main thread in play()
		}

		__clock = 0;
		bitmap.onFormatSetup.dispatch();
		bitmap.onOpening.dispatch();
		return true;
	}

	public function play():Bool {
		if (!__open) {
			// a video that failed to load "ends" right away, so scripts waiting for it carry on
			bitmap.onEndReached.dispatch();
			return false;
		}
		__playing = true;
		__paused = false;
		if (__sound == null && __audio != null) {
			__sound = new FlxSound().loadEmbedded(Sound.fromAudioBuffer(__audio), __loop);
			FlxG.sound.list.add(__sound);
		}
		if (__sound != null && visible) __sound.play(true, __clock * 1000);
		bitmap.onPlaying.dispatch();
		return true;
	}

	public function stop():Void {
		__playing = false;
		__paused = false;
		if (__sound != null) __sound.stop();
		__seek(0);
		bitmap.onStopped.dispatch();
	}

	public function pause():Void {
		if (!__playing || __paused) return;
		__paused = true;
		if (__sound != null) __sound.pause();
		bitmap.onPaused.dispatch();
	}

	public function resume():Void {
		if (!__playing || !__paused) return;
		__paused = false;
		if (__sound != null) __sound.resume();
	}

	public function togglePaused():Void __paused ? resume() : pause();

	override function update(elapsed:Float):Void {
		super.update(elapsed);
		if (!__open || !__playing || __paused) return;
		// Hidden videos that were never shown wait at the start (mods hide clips until their
		// section and pin them with bitmap.time = 0); this also keeps them from decoding all song.
		if (!__everShown) {
			if (!visible) {
				if (__sound != null && __sound.playing) __sound.pause();
				return;
			}
			__everShown = true;
			if (__sound != null) __sound.play(true, __clock * 1000);
		}
		if (__held) {
			if (!visible) return;
			// a script kept this hidden video at the start; it begins now that it is shown
			__held = false;
			if (__sound != null) __sound.play(true, 0);
		}

		if (__sound != null && __sound.playing) __clock = __sound.time / 1000;
		else __clock += elapsed;

		if (untyped __cpp__("cnev_frame({0}, {1}, (unsigned char*){2}->GetBase())", __handle, __clock, __frameBytes.getData()) == 1)
			__image.version++;


		if (__duration > 0 && __clock >= __duration - 0.02) {
			if (__loop) {
				__seek(0);
				if (__sound != null) __sound.play(true, 0);
			} else {
				__playing = false;
				bitmap.onEndReached.dispatch();
			}
		}
	}

	@:noCompletion function __seek(seconds:Float) {
		__clock = seconds;
		if (__open) untyped __cpp__("cnev_seek({0}, {1})", __handle, seconds);
	}

	@:noCompletion function __getTime():Int return Std.int(__clock * 1000);

	@:noCompletion function __setTime(ms:Int):Int {
		var seconds = ms / 1000;
		if (ms == 0 && !visible) {
			// scripts pin hidden videos to the start every frame; seek once and hold there
			if (!__held || __clock != 0) {
				__seek(0);
				if (__sound != null) __sound.pause();
				__held = true;
			}
			return ms;
		}
		if (Math.abs(seconds - __clock) < 0.03) return ms;
		__seek(seconds);
		if (__sound != null && __sound.playing) __sound.time = ms;
		return ms;
	}

	@:noCompletion function __close() {
		if (__sound != null) {
			__sound.stop();
			FlxG.sound.list.remove(__sound);
			__sound.destroy();
			__sound = null;
		}
		__audio = null;
		__held = false;
		__everShown = false;
		if (__open) {
			untyped __cpp__("cnev_close({0})", __handle);
			__handle = null;
			__open = false;
		}
		__playing = false;
	}

	override function destroy():Void {
		__close();
		if (bitmap != null) bitmap.destroy();
		super.destroy();
	}

	@:allow(hxvlc.flixel.FlxVideoBitmap) function get_duration():Float return __duration;
	@:allow(hxvlc.flixel.FlxVideoBitmap) function get_isPlaying():Bool return __playing && !__paused;
	@:allow(hxvlc.flixel.FlxVideoBitmap) function get_volumeValue():Float return __sound != null ? __sound.volume : 1;
	@:allow(hxvlc.flixel.FlxVideoBitmap) function set_volumeValue(v:Float):Float {
		if (__sound != null) __sound.volume = v;
		return v;
	}
}

/** Mirrors the parts of hxvlc's `Video` bitmap that mod scripts commonly touch. */
@:keep
@:access(hxvlc.flixel.FlxVideoSprite)
class FlxVideoBitmap {
	var owner:FlxVideoSprite;

	public var time(get, set):Int;
	public var length(get, never):Int;
	public var duration(get, never):Int;
	public var position(get, set):Float;
	public var isPlaying(get, never):Bool;
	public var volume(get, set):Int;
	public var rate:Float = 1;
	public var mute:Bool = false;
	public var isSeekable:Bool = true;
	public var bitmapData(get, never):BitmapData;

	public var onOpening(default, null):FlxSignal = new FlxSignal();
	public var onPlaying(default, null):FlxSignal = new FlxSignal();
	public var onStopped(default, null):FlxSignal = new FlxSignal();
	public var onPaused(default, null):FlxSignal = new FlxSignal();
	public var onEndReached(default, null):FlxSignal = new FlxSignal();
	public var onEncounteredError(default, null):FlxTypedSignal<String->Void> = new FlxTypedSignal<String->Void>();
	public var onFormatSetup(default, null):FlxSignal = new FlxSignal();

	public function new(owner:FlxVideoSprite) this.owner = owner;

	function get_time():Int return owner.__getTime();
	function set_time(v:Int):Int return owner.__setTime(v);
	function get_length():Int return Std.int(owner.get_duration() * 1000);
	function get_duration():Int return get_length();
	function get_position():Float return owner.get_duration() > 0 ? owner.__clock / owner.get_duration() : 0;
	function get_isPlaying():Bool return owner.get_isPlaying();
	function set_position(v:Float):Float {
		owner.__setTime(Std.int(v * owner.get_duration() * 1000));
		return v;
	}
	function get_bitmapData():BitmapData return owner.graphic != null ? owner.graphic.bitmap : null;
	function get_volume():Int return Std.int(owner.get_volumeValue() * 100);
	function set_volume(v:Int):Int { owner.set_volumeValue(v / 100); return v; }

	public function destroy():Void {
		for (s in [onOpening, onPlaying, onStopped, onPaused, onEndReached, onFormatSetup]) s.destroy();
		onEncounteredError.destroy();
	}
}
