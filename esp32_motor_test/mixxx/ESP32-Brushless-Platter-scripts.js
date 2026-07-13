var ESP32BrushlessPlatter = {};

ESP32BrushlessPlatter.playCc = 0x14; // CC20, sent from Mixxx back to the Python bridge.
ESP32BrushlessPlatter.lastPlay = -1;
ESP32BrushlessPlatter.deck = 1;
ESP32BrushlessPlatter.group = "[Channel1]";
ESP32BrushlessPlatter.ticksPerRev = 96;
ESP32BrushlessPlatter.rpm = 33 + 1 / 3;
ESP32BrushlessPlatter.alpha = 1 / 8;
ESP32BrushlessPlatter.beta = ESP32BrushlessPlatter.alpha / 32;
ESP32BrushlessPlatter.scratchTimer = 0;
ESP32BrushlessPlatter.scratchReleaseMs = 120;
ESP32BrushlessPlatter.scale = 1.0;
ESP32BrushlessPlatter.nudgeTimer = 0;
ESP32BrushlessPlatter.nudgeReleaseMs = 160;

ESP32BrushlessPlatter.init = function() {
    engine.connectControl("[Channel1]", "play", "ESP32BrushlessPlatter.onPlay");
    ESP32BrushlessPlatter.onPlay(engine.getValue("[Channel1]", "play"));
};

ESP32BrushlessPlatter.shutdown = function() {
    midi.sendShortMsg(0xB0, ESP32BrushlessPlatter.playCc, 0);
    ESP32BrushlessPlatter.releaseNudge();
    if (engine.isScratching(ESP32BrushlessPlatter.deck)) {
        engine.scratchDisable(ESP32BrushlessPlatter.deck);
    }
};

ESP32BrushlessPlatter.onPlay = function(value) {
    var playing = value > 0 ? 1 : 0;
    if (playing === ESP32BrushlessPlatter.lastPlay) {
        return;
    }
    ESP32BrushlessPlatter.lastPlay = playing;
    if (playing) {
        if (ESP32BrushlessPlatter.scratchTimer) {
            engine.stopTimer(ESP32BrushlessPlatter.scratchTimer);
            ESP32BrushlessPlatter.scratchTimer = 0;
        }
        if (engine.isScratching(ESP32BrushlessPlatter.deck)) {
            engine.scratchDisable(ESP32BrushlessPlatter.deck);
        }
        ESP32BrushlessPlatter.releaseNudge();
    }
    midi.sendShortMsg(0xB0, ESP32BrushlessPlatter.playCc, playing ? 127 : 0);
};

ESP32BrushlessPlatter.jog = function(channel, control, value, status, group) {
    var delta = value - 64;
    if (delta === 0) {
        return;
    }

    if (engine.getValue(ESP32BrushlessPlatter.group, "play") > 0) {
        var down = delta < 0 ? 1 : 0;
        engine.setValue(ESP32BrushlessPlatter.group, "rate_temp_down", down);
        engine.setValue(ESP32BrushlessPlatter.group, "rate_temp_up", down ? 0 : 1);
        if (ESP32BrushlessPlatter.nudgeTimer) {
            engine.stopTimer(ESP32BrushlessPlatter.nudgeTimer);
        }
        ESP32BrushlessPlatter.nudgeTimer = engine.beginTimer(
            ESP32BrushlessPlatter.nudgeReleaseMs,
            "ESP32BrushlessPlatter.releaseNudge",
            true
        );
        return;
    }

    if (!engine.isScratching(ESP32BrushlessPlatter.deck)) {
        engine.scratchEnable(
            ESP32BrushlessPlatter.deck,
            ESP32BrushlessPlatter.ticksPerRev,
            ESP32BrushlessPlatter.rpm,
            ESP32BrushlessPlatter.alpha,
            ESP32BrushlessPlatter.beta,
            true
        );
    }

    engine.scratchTick(ESP32BrushlessPlatter.deck, delta * ESP32BrushlessPlatter.scale);

    if (ESP32BrushlessPlatter.scratchTimer) {
        engine.stopTimer(ESP32BrushlessPlatter.scratchTimer);
    }
    ESP32BrushlessPlatter.scratchTimer = engine.beginTimer(
        ESP32BrushlessPlatter.scratchReleaseMs,
        "ESP32BrushlessPlatter.releaseScratch",
        true
    );
};

ESP32BrushlessPlatter.releaseScratch = function() {
    ESP32BrushlessPlatter.scratchTimer = 0;
    if (engine.isScratching(ESP32BrushlessPlatter.deck)) {
        engine.scratchDisable(ESP32BrushlessPlatter.deck);
    }
};

ESP32BrushlessPlatter.releaseNudge = function() {
    ESP32BrushlessPlatter.nudgeTimer = 0;
    engine.setValue(ESP32BrushlessPlatter.group, "rate_temp_down", 0);
    engine.setValue(ESP32BrushlessPlatter.group, "rate_temp_up", 0);
};
