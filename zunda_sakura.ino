/*
  zunda_sakura.ino
  入力した会話音声をずんだもん語に変換するデバイス
  for M5Stack ATOMS3R + ATOMIC ECHO BASE

  Copyright (c) 2026 Kaz  (https://akibabara.com/blog/)
  Released under the MIT license.
  see https://opensource.org/licenses/MIT

  ※本プログラムはMITライセンスですが（個別に記載があるものを除く）、
  音声データ、画像データ、およびキャラクターに関しては、それぞれの
  ライセンス・利用規約が適用されます。
*/
#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// 基本設定（各自の環境に合わせて変更してください）
#define WIFI_SSID     "****"
#define WIFI_PASSWORD "****"
const String SAKURA_APIKEY = "****"; // 有料
const int SPEAKER_VOLUME = 70;    // スピーカー音量（0-255、100%は爆音なので注意）

// さくらのAI Engine設定
const String TTS_ENDPOINT = "https://api.ai.sakura.ad.jp/tts/v1";
const String TTS_APIKEY = SAKURA_APIKEY;
const int VOICEVOX_SPEAKER = 3;   // 音声合成モデル（ずんだもん あまあま）
const String STT_ENDPOINT = "https://api.ai.sakura.ad.jp/v1";
const String STT_MODEL = "whisper-large-v3-turbo";  // 音声認識用モデル
const String STT_APIKEY = SAKURA_APIKEY;
const String CHAT_ENDPOINT = "https://api.ai.sakura.ad.jp/v1";
const String CHAT_MODEL = "gpt-oss-120b";  // CHAT用モデル
// const String CHAT_MODEL = "Qwen3-Coder-30B-A3B-Instruct"; // コーディング用モデル
// const String CHAT_MODEL = "llm-jp-3.1-8x13b-instruct4";  // このモデルは指示に従わないから不向き
const String CHAT_APIKEY = SAKURA_APIKEY;
const int CHAT_TTS_MAX_CHARS = 300;      // TTSへ渡す文字数の上限

// システムプロンプト
const String CHAT_SYSTEM_PROMPT =
  "ユーザーが入力した文章を、ずんだもんの口調に変更して出力してください。"
  "ずんだもんは語尾に「～のだ」「～なのだ」と付ける言葉遣いが特徴です。"
  "（例: 天気です→天気なのだ、ありました→あったのだ、行きます→行くのだ、"
  "どうしますか？→どうするのだ？、いいですね→いいのだ、わかりません→わからないのだ）"
  "ユーザーが入力したテキストを変換した内容だけ出力してください。";

// アバター設定
const uint32_t BLINK_TIME_BASE = 2000;  // まばたき時間　基本間隔
const uint32_t BLINK_TIME_RAND = 2000;  // まばたき時間　ランダム追加 max
const uint32_t BLINK_TIME_CLOS = 150;   // まばたき時間　閉じてる時間
const uint32_t KYORO_TIME_BASE = 1500;  // キョロキョロ時間　基本間隔
const uint32_t KYORO_TIME_RMAX = 2000;  // キョロキョロ時間　ランダム追加 max

// GPIO設定 ATOMS3R
// #define GPIO_SDA  38   // SDA (M5Unifiedのatomic_echoで使用)
// #define GPIO_SCL  39   // SCL

// アバター関連
#include "Zundavatar.h"   // アバタークラス
using namespace zundavatar;
bool lcdBusy = false;   // ディスプレイの排他制御用　クラス間で共有される
Zundavatar avatar(&lcdBusy);

// アバター画像データ
#include "data_image_zundamon128.h"   // 画像データ　ずんだもん

// 音声データ
// #include "data_sound_zundamon.h"  // 音声データ　ずんだもん
// const unsigned char* soundData[] = { sound000, sound001, sound002 };
// const size_t sizeData[] = { sizeof(sound000), sizeof(sound001), sizeof(sound002) };

// 割り込み処理
#include <Ticker.h>
Ticker tickerk;
volatile bool refresh;

// 各種定数設定
#define WAV_HEADER_SIZE 44  // WAVヘッダー：PCMデータ開始オフセット（標準44バイト）
#define SAMPLE_RATE 24000  // 音声サンプルレート
const int LIPSYNC_DURATION_MS = 200;  // リップシンクの更新間隔
const int LIPSYNC_JITTER_PERCENT = 20;  // リップシンク間隔の揺らぎ幅（±%）
const int MAX_WAV_BYTES = 3 * 1024 * 1024;  // 受信/生成するWAVの上限バイト数（PSRAM使用）
const int WAV_PREBUFFER_BYTES = 50 * 1024; // 再生開始前の事前受信バイト数
const uint32_t STT_SAMPLE_RATE = 16000;      // STT録音のサンプルレート Hz
const uint32_t STT_MAX_RECORD_MS = 30000;   // 最大録音時間 ms
const uint32_t STT_SILENCE_TIMEOUT_MS = 1200;        // 無音が続いたら録音停止するまでの時間 ms
const uint32_t STT_NO_VOICE_TIMEOUT_MS = 5000;       // 発話未検出のまま録音を打ち切る時間 ms
const uint32_t STT_MIN_RECORD_AFTER_VOICE_MS = 1200; // 発話検出後に最低限録音を続ける時間 ms
const uint32_t STT_RECORD_START_IGNORE_MS = 180;  // ボタン押下直後のノイズ無視時間 ms
const uint32_t STT_MIC_SETTLE_MS = 60;            // スピーカー停止後の安定待ち ms
const int STT_SILENCE_THRESHOLD = 240;             // 有声音判定のしきい値（平均振幅）
const uint8_t STT_MIC_MAGNIFICATION = 8;           // マイク入力ゲイン倍率
const size_t STT_BLOCK_SAMPLES = 256;              // 1回のマイク読み取りサンプル数

struct LipsyncState {
  bool enabled;
  uint32_t startMs;
  uint32_t lastUpdateMs;
  uint32_t nextIntervalMs;
};
using BeforeSttCallback = void (*)();
using BeforePlayCallback = void (*)();

// グローバル変数
M5Canvas tgauge;  // アバターにオーバーレイするスプライト
bool blink = false;
bool kyoro = false;
uint16_t blinkOpen = 1;   // 通常目のインデックス番号

// デバッグに便利なマクロ定義 -- M5.Log置き換えver --------
// 参考ページ https://github.com/lovyan03/M5Unified_HandsOn/blob/main/sample_code/Log1.cpp
#define sp(x) M5.Log.println(String(x).c_str())
#define spn(x) M5.Log.print(String(x).c_str())
#define spp(k,v) M5.Log.println(String(String(k)+"="+String(v)).c_str())
#define spf(fmt, ...) M5.Log.printf(fmt, __VA_ARGS__)
#define array_length(x) (sizeof(x) / sizeof(x[0]))


// ====================================================================================

// アバターのパーツをデフォルトに設定する
void defaultAvatarParts() {
  avatar.changeParts("body", 0);    // 体
  avatar.changeParts("rhand", 0);   // 右腕
  avatar.changeParts("lhand", 0);   // 左腕
  avatar.changeParts("eyebrow", 0); // 眉毛
  avatar.changeParts("eye", 1);     // 目
  avatar.changeParts("mouth", 0);   // 口
  avatar.changeParts("ahiru", 0);   // アヒル
}

// 指定時間待機する。待機中もM5.update()を行っており、ボタンが押されたら即座に抜ける
bool waitMillis(uint32_t ms) {
  bool res = false;
  uint32_t tmwait = millis() + ms;
  while (millis() < tmwait) {
    M5.update();
    if (M5.BtnA.isPressed()) {
      res = true;
      break;
    }
    delay(10);
  }
  return res;
}

// 音声バッファをPSRAMに確保する（PSRAM必須）
uint8_t* allocateAudioBuffer(size_t size, bool* usedPsram) {
  *usedPsram = false;
  uint8_t* wavBuf = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  if (wavBuf) {
    *usedPsram = true;
    return wavBuf;
  }
  return nullptr;
}

// 音声バッファを解放する（本実装ではPSRAM確保のみ）
void freeAudioBuffer(uint8_t* wavBuf, bool usedPsram) {
  if (!wavBuf) return;
  if (usedPsram) heap_caps_free(wavBuf);
  else free(wavBuf);
}

// STT用バッファをPSRAMに確保する（PSRAM必須）
uint8_t* allocateSttBuffer(size_t size, bool* usedPsram) {
  *usedPsram = false;
  uint8_t* buf = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  if (buf) {
    *usedPsram = true;
    return buf;
  }
  return nullptr;
}

// 平均音量を口形（母音）に変換してリップシンクへ反映する
void applyLipsyncByVolume(int averageVolume) {
  if (averageVolume > 1000)     avatar.setLipsyncVowel(5, 80);  // "o" 80ms
  else if (averageVolume > 100) avatar.setLipsyncVowel(2, 80);  // "a" 80ms
  else                          avatar.setLipsyncVowel(0, 1);   // "n" 1ms
}

// 再生経過時間に対応するWAV区間の平均音量を計算する
// readableBytesは現在安全に参照できる受信済み/確保済みバイト数
int calcAverageVolumeFromWav(const uint8_t* wavBuf, int readableBytes, uint32_t playElapsedMs) {
  int idx = WAV_HEADER_SIZE + (int)(((uint64_t)playElapsedMs * SAMPLE_RATE) / 1000) * 2;
  int minWindowMs = LIPSYNC_DURATION_MS * (100 - LIPSYNC_JITTER_PERCENT) / 100;
  int numSamples = (minWindowMs * SAMPLE_RATE) / 1000;
  long totalVolume = 0;
  int totalSamples = 0;

  for (int i = 0; i < numSamples; i++) {
    int pos = idx + i * 2;
    if ((pos + 1) >= readableBytes) break;
    int16_t val = wavBuf[pos] | (wavBuf[pos + 1] << 8);
    totalVolume += abs((int)val);
    totalSamples++;
  }
  return (totalSamples > 0) ? (totalVolume / totalSamples) : 0;
}

// 次回のリップシンク更新間隔を、基準値の±20%でランダムに決める
uint32_t nextLipsyncIntervalMs() {
  int minMs = LIPSYNC_DURATION_MS * (100 - LIPSYNC_JITTER_PERCENT) / 100;
  int maxMs = LIPSYNC_DURATION_MS * (100 + LIPSYNC_JITTER_PERCENT) / 100;
  return (uint32_t)random(minMs, maxMs + 1);
}

// 指定間隔ごとにリップシンクを更新する（未到達の間隔では何もしない）
void updateLipsyncFromWav(LipsyncState& lipsync, const uint8_t* wavBuf, int readableBytes) {
  if (!lipsync.enabled) return;
  uint32_t now = millis();
  if ((now - lipsync.lastUpdateMs) < lipsync.nextIntervalMs) return;

  int averageVolume = calcAverageVolumeFromWav(wavBuf, readableBytes, now - lipsync.startMs);
  applyLipsyncByVolume(averageVolume);
  lipsync.lastUpdateMs = now;
  lipsync.nextIntervalMs = nextLipsyncIntervalMs();
}

// リップシンク終了時に口を閉じ状態へ戻す
void stopLipsync(const LipsyncState& lipsync) {
  if (lipsync.enabled) avatar.setLipsyncVowel(0, 1);
}

// URLエンコード（audio_queryのtextパラメータ用）
String urlEncode(const char* msg) {
  const char* hex = "0123456789ABCDEF";
  String encoded = "";
  while (*msg != '\0') {
    char c = *msg;
    if (('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || ('0' <= c && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else {
      encoded += '%';
      encoded += hex[(uint8_t)c >> 4];
      encoded += hex[(uint8_t)c & 0x0F];
    }
    msg++;
  }
  return encoded;
}

// HTTPレスポンスのWAVストリームを受信しながら再生する
// GET/POSTの違いは呼び出し側で吸収し、この関数はWAV受信と再生だけを担当する
bool streamWavFromHttpAndPlay(HTTPClient& http, int contentLength, bool withlip, BeforePlayCallback beforePlayCallback = nullptr) {
  // まずレスポンスサイズを検証し、サイズ不明モードかどうかを決める
  spf("contentLength=%d\n", contentLength);
  const bool lengthKnown = (contentLength > 0);
  if (lengthKnown && contentLength > MAX_WAV_BYTES) {
    spf("Invalid or too large content: %d\n", contentLength);
    spf("HTTP connected=%d\n", http.connected() ? 1 : 0);
    spf("Header Content-Type=%s\n", http.header("Content-Type").c_str());
    spf("Header Content-Length=%s\n", http.header("Content-Length").c_str());
    spf("Header Transfer-Encoding=%s\n", http.header("Transfer-Encoding").c_str());
    spf("Header Connection=%s\n", http.header("Connection").c_str());
    return false;
  }
  if (!lengthKnown) {
    sp("contentLength unknown: fallback to read-until-end mode");
  }

  // 再生用バッファを確保する（サイズ不明時は上限サイズで確保）
  const int bufferCapacity = lengthKnown ? contentLength : MAX_WAV_BYTES;

  bool used_psram = false;
  uint8_t* wavBuf = allocateAudioBuffer(bufferCapacity, &used_psram);
  if (!wavBuf) {
    sp("Buffer alloc failed");
    return false;
  }
  if (!lengthKnown) {
    // サイズ不明時に未受信領域を読んでもノイズになりにくいようゼロ初期化
    memset(wavBuf, 0, bufferCapacity);
  }

  // 受信状態と再生状態を管理するワーク変数を初期化する
  const size_t CHUNK_SIZE = 2048;
  int len = 0;
  uint32_t lastDataMs = millis();
  bool started = false;
  int expectedPlayLength = lengthKnown ? contentLength : -1;
  LipsyncState lipsync = { withlip, 0, 0, nextLipsyncIntervalMs() };

  // HTTPストリームを取得し、使える状態かを確認する
  WiFiClient* stream = http.getStreamPtr();
  if (!stream) {
    sp("HTTP stream is null");
    freeAudioBuffer(wavBuf, used_psram);
    return false;
  }
  spf("HTTP stream ready: connected=%d available=%d\n", http.connected() ? 1 : 0, (int)stream->available());

  // 必要なら再生開始前コールバックを呼ぶ
  if (beforePlayCallback) {
    beforePlayCallback();  // Play前のコールバック
  }

  // メインループ：受信しながら条件を満たしたら再生開始する
  while (lengthKnown ? (len < contentLength) : true) {
    M5.update();

    size_t avail = stream->available();
    if (avail > 0) {
      size_t toRead = avail < CHUNK_SIZE ? avail : CHUNK_SIZE;
      size_t remain = bufferCapacity - len;
      if (remain == 0) {
        spf("buffer full at len=%d (capacity=%d)\n", len, bufferCapacity);
        break;
      }
      if (toRead > remain) toRead = remain;
      int n = stream->readBytes(wavBuf + len, toRead);
      if (n > 0) {
        len += n;
        lastDataMs = millis();
      }
    } else {
      if (!http.connected()) {
        spf("stream disconnected at len=%d\n", len);
        break;
      }
      if ((millis() - lastDataMs) > 30000) {  // 30秒無通信で中断
        spf("stream timeout at len=%d\n", len);
        break;
      }
      delay(1);
      continue;
    }

    // サイズ不明時はWAVヘッダから最終サイズを推定する
    if (!started && !lengthKnown && expectedPlayLength < 0 && len >= WAV_HEADER_SIZE) {
      if (memcmp(wavBuf, "RIFF", 4) == 0 && memcmp(wavBuf + 8, "WAVE", 4) == 0) {
        uint32_t dataBytes = (uint32_t)wavBuf[40]
                           | ((uint32_t)wavBuf[41] << 8)
                           | ((uint32_t)wavBuf[42] << 16)
                           | ((uint32_t)wavBuf[43] << 24);
        uint32_t totalBytes = WAV_HEADER_SIZE + dataBytes;
        if (totalBytes > WAV_HEADER_SIZE && totalBytes <= (uint32_t)bufferCapacity) {
          expectedPlayLength = (int)totalBytes;
          spf("WAV length from header=%d bytes\n", expectedPlayLength);
        }
      }
    }

    // サイズ既知のときは一定量受信後に再生開始する
    if (!started && lengthKnown && len >= WAV_PREBUFFER_BYTES) {
      M5.Speaker.begin();
      M5.Speaker.setVolume(SPEAKER_VOLUME);
      spf("start play at %d bytes\n", len);
      bool playOk = M5.Speaker.playWav(wavBuf, contentLength, 1, 0, true);
      if (!playOk) {
        sp("playWav failed (WAV format?)");
        freeAudioBuffer(wavBuf, used_psram);
        return false;
      }
      started = true;
      lipsync.startMs = millis();
      lipsync.lastUpdateMs = lipsync.startMs;
      lipsync.nextIntervalMs = nextLipsyncIntervalMs();
    }
    // サイズ不明でも推定サイズが取れたら同様に先行再生する
    if (!started && !lengthKnown && expectedPlayLength > 0 && len >= WAV_PREBUFFER_BYTES) {
      M5.Speaker.begin();
      M5.Speaker.setVolume(SPEAKER_VOLUME);
      spf("start play(unknown length) at %d bytes target=%d\n", len, expectedPlayLength);
      bool playOk = M5.Speaker.playWav(wavBuf, expectedPlayLength, 1, 0, true);
      if (!playOk) {
        sp("playWav failed (unknown length mode)");
        freeAudioBuffer(wavBuf, used_psram);
        return false;
      }
      started = true;
      lipsync.startMs = millis();
      lipsync.lastUpdateMs = lipsync.startMs;
      lipsync.nextIntervalMs = nextLipsyncIntervalMs();
    }

    // 再生中は受信済み領域を使ってリップシンクを更新する
    if (started && M5.Speaker.isPlaying(0)) {
      updateLipsyncFromWav(lipsync, wavBuf, len);  // 未受信領域は参照しない
    }
  }
  // 受信終了時点の実サイズを確定する
  if (!lengthKnown) {
    contentLength = len;
    spf("len=%d (expected unknown)\n", len);
  } else {
    spf("len=%d (expected %d)\n", len, contentLength);
  }

  // 事前再生していない場合は、受信完了後にまとめて再生する
  if (!started && len > WAV_HEADER_SIZE) {
    M5.Speaker.begin();
    M5.Speaker.setVolume(SPEAKER_VOLUME);
    spf("start play after download at %d bytes\n", len);
    bool playOk = M5.Speaker.playWav(wavBuf, len, 1, 0, true);
    if (!playOk) {
      sp("playWav failed after download (WAV format?)");
      freeAudioBuffer(wavBuf, used_psram);
      return false;
    }
    started = true;
    lipsync.startMs = millis();
    lipsync.lastUpdateMs = lipsync.startMs;
    lipsync.nextIntervalMs = nextLipsyncIntervalMs();
  }

  // 再生開始できなければ失敗とする
  if (!started) {
    sp("Prebuffer not reached");
    freeAudioBuffer(wavBuf, used_psram);
    return false;
  }

  // サイズ既知モードでは受信不足をエラー扱いにする
  if (lengthKnown && (len != contentLength)) {
    spf("Download incomplete: %d/%d\n", len, contentLength);
    M5.Speaker.stop(0);
    freeAudioBuffer(wavBuf, used_psram);
    return false;
  }

  // 再生終了までリップシンクを継続する
  while (M5.Speaker.isPlaying(0)) {
    M5.update();
    updateLipsyncFromWav(lipsync, wavBuf, len);
    delay(5);
  }
  sp("finish play");

  // 後始末（口のリセット、音量ミュート、バッファ解放）
  stopLipsync(lipsync);
  M5.Speaker.setVolume(0);
  freeAudioBuffer(wavBuf, used_psram);
  return true;
}

// 喋ると同時にリップシンクを行う（M5.Speaker.playWav使用）
/*
void playVoiceFromProgmem(int no, bool withlip) {
  uint32_t stams = millis();

  // PROGMEMのWAVデータをPSRAMにコピー（M5.SpeakerのDMAがフラッシュを直接読めないため）
  bool used_psram = false;
  uint8_t* wavBuf = allocateAudioBuffer(sizeData[no], &used_psram);
  if (!wavBuf) return;
  memcpy_P(wavBuf, soundData[no], sizeData[no]);

  // 再生開始（M5.Speaker.playWavはWAVヘッダー付きデータを受け取る）
  M5.Speaker.setVolume(SPEAKER_VOLUME);
  M5.Speaker.playWav(wavBuf, sizeData[no], 1, 0, true);
  LipsyncState lipsync = { withlip, stams, 0 };

  while (M5.Speaker.isPlaying(0)) {
    M5.update();
    updateLipsyncFromWav(lipsync, wavBuf, (int)sizeData[no]);
    delay(5);
  }

  stopLipsync(lipsync);
  M5.Speaker.setVolume(0);
  freeAudioBuffer(wavBuf, used_psram);
}
*/

// URLからWAVをダウンロードして再生（24kHz, 16bit, モノラル想定）
bool playVoiceFromUrl(const char* url, bool withlip) {
  sp("debug playVoiceFromUrl() start");

  // HTTP GETを実行し、ヘッダ情報を回収する
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();  // 証明書検証をスキップ
  http.useHTTP10(true);  // chunked回避のためHTTP/1.0を使用
  const char* headerKeys[] = { "Content-Type", "Content-Length", "Transfer-Encoding", "Connection" };
  http.collectHeaders(headerKeys, 4);
  http.begin(client, url);
  http.setTimeout(10000);
  int httpCode = http.GET();
  spf("httpCode=%d\n", httpCode);

  // HTTPエラー時は早期リターンする
  if (httpCode != HTTP_CODE_OK) {
    spf("HTTP GET failed: %d", httpCode);
    http.end();
    return false;
  }
  // 共通関数に委譲してWAV受信・再生を行う
  int contentLength = http.getSize();
  spf("URL WAV headers: type=%s len=%s te=%s conn=%s\n",
    http.header("Content-Type").c_str(),
    http.header("Content-Length").c_str(),
    http.header("Transfer-Encoding").c_str(),
    http.header("Connection").c_str());
  bool result = streamWavFromHttpAndPlay(http, contentLength, withlip);
  http.end();
  return result;
}

// VOICEVOX /audio_query を呼び出し、合成パラメータJSONを取得する
bool requestVoicevoxAudioQuery(const char* text, int speaker, String& outQueryJson) {
  // audio_query用URLを生成する
  String queryUrl = TTS_ENDPOINT + "/audio_query?text=" + urlEncode(text) + "&speaker=" + String(speaker);
  spf("audio_query url=%s\n", queryUrl.c_str());

  // VOICEVOX audio_queryへPOSTしてJSONを取得する
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.useHTTP10(true);
  const char* headerKeys[] = { "Content-Type", "Content-Length", "Transfer-Encoding", "Connection" };
  http.collectHeaders(headerKeys, 4);
  http.begin(client, queryUrl);
  http.setTimeout(15000);
  http.addHeader("Accept", "application/json");
  http.addHeader("Authorization", String("Bearer ") + TTS_APIKEY);
  int httpCode = http.POST((uint8_t*)"", 0);  // body空でPOST
  spf("audio_query code=%d\n", httpCode);

  // HTTPエラー時は失敗で返す
  if (httpCode != HTTP_CODE_OK) {
    spf("audio_query failed: %d", httpCode);
    http.end();
    return false;
  }

  // 取得したJSON本文を返却する
  outQueryJson = http.getString();
  spf("audio_query headers: type=%s len=%s te=%s\n",
    http.header("Content-Type").c_str(),
    http.header("Content-Length").c_str(),
    http.header("Transfer-Encoding").c_str());
  spf("audio_query body length=%d\n", outQueryJson.length());
  http.end();
  if (outQueryJson.length() == 0) {
    sp("audio_query response is empty");
    return false;
  }
  return true;
}

// textからVOICEVOXで音声合成し、WAVを再生する
// 手順: /audio_query -> /synthesis
bool playVoiceFromText(const char* text, bool withlip, BeforePlayCallback beforePlayCallback = nullptr) {
  // 事前条件（入力文字列）を確認する
  sp("debug playVoiceFromText() start");
  if (!text || text[0] == '\0') {
    sp("text is empty");
    return false;
  }

  // audio_queryで合成パラメータJSONを取得する
  String queryJson;
  if (!requestVoicevoxAudioQuery(text, VOICEVOX_SPEAKER, queryJson)) {
    return false;
  }

  // synthesisへPOSTし、戻りWAVを共通再生関数へ渡す
  String synthesisUrl = TTS_ENDPOINT + "/synthesis?speaker=" + String(VOICEVOX_SPEAKER);
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.useHTTP10(true);
  const char* headerKeys[] = { "Content-Type", "Content-Length", "Transfer-Encoding", "Connection" };
  http.collectHeaders(headerKeys, 4);
  http.begin(client, synthesisUrl);
  http.setTimeout(30000);
  http.addHeader("Accept", "audio/wav,*/*");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + TTS_APIKEY);
  spf("synthesis url=%s\n", synthesisUrl.c_str());
  spf("synthesis post bytes=%d\n", queryJson.length());
  int httpCode = http.POST((uint8_t*)queryJson.c_str(), queryJson.length());
  spf("synthesis code=%d\n", httpCode);

  // synthesis失敗時は中断する
  if (httpCode != HTTP_CODE_OK) {
    spf("synthesis failed: %d", httpCode);
    http.end();
    return false;
  }

  // ヘッダをログしつつ、受信・再生処理を実行する
  int contentLength = http.getSize();
  spf("synthesis headers: type=%s len=%s te=%s conn=%s\n",
    http.header("Content-Type").c_str(),
    http.header("Content-Length").c_str(),
    http.header("Transfer-Encoding").c_str(),
    http.header("Connection").c_str());
  bool result = streamWavFromHttpAndPlay(http, contentLength, withlip, beforePlayCallback);
  http.end();
  return result;
}

// PCM16モノラルのWAVヘッダを書き込む
void writeWavHeader(uint8_t* wav, uint32_t pcmBytes, uint32_t sampleRate, uint16_t channels, uint16_t bitsPerSample) {
  uint32_t byteRate = sampleRate * channels * (bitsPerSample / 8);
  uint16_t blockAlign = channels * (bitsPerSample / 8);
  uint32_t riffSize = 36 + pcmBytes;

  memcpy(wav + 0, "RIFF", 4);
  wav[4] = (uint8_t)(riffSize & 0xFF);
  wav[5] = (uint8_t)((riffSize >> 8) & 0xFF);
  wav[6] = (uint8_t)((riffSize >> 16) & 0xFF);
  wav[7] = (uint8_t)((riffSize >> 24) & 0xFF);
  memcpy(wav + 8, "WAVEfmt ", 8);
  wav[16] = 16; wav[17] = 0; wav[18] = 0; wav[19] = 0;  // fmt chunk size
  wav[20] = 1; wav[21] = 0;                               // PCM format
  wav[22] = (uint8_t)(channels & 0xFF);
  wav[23] = (uint8_t)((channels >> 8) & 0xFF);
  wav[24] = (uint8_t)(sampleRate & 0xFF);
  wav[25] = (uint8_t)((sampleRate >> 8) & 0xFF);
  wav[26] = (uint8_t)((sampleRate >> 16) & 0xFF);
  wav[27] = (uint8_t)((sampleRate >> 24) & 0xFF);
  wav[28] = (uint8_t)(byteRate & 0xFF);
  wav[29] = (uint8_t)((byteRate >> 8) & 0xFF);
  wav[30] = (uint8_t)((byteRate >> 16) & 0xFF);
  wav[31] = (uint8_t)((byteRate >> 24) & 0xFF);
  wav[32] = (uint8_t)(blockAlign & 0xFF);
  wav[33] = (uint8_t)((blockAlign >> 8) & 0xFF);
  wav[34] = (uint8_t)(bitsPerSample & 0xFF);
  wav[35] = (uint8_t)((bitsPerSample >> 8) & 0xFF);
  memcpy(wav + 36, "data", 4);
  wav[40] = (uint8_t)(pcmBytes & 0xFF);
  wav[41] = (uint8_t)((pcmBytes >> 8) & 0xFF);
  wav[42] = (uint8_t)((pcmBytes >> 16) & 0xFF);
  wav[43] = (uint8_t)((pcmBytes >> 24) & 0xFF);
}

// JSON文字列から "text" フィールドを簡易抽出する
bool extractJsonStringField(const String& json, const char* key, String& out) {
  String token = "\"" + String(key) + "\":\"";
  int start = json.indexOf(token);
  if (start < 0) return false;
  start += token.length();

  String value = "";
  bool escape = false;
  for (int i = start; i < json.length(); ++i) {
    char c = json[i];
    if (escape) {
      if (c == 'n') value += '\n';
      else if (c == 'r') value += '\r';
      else if (c == 't') value += '\t';
      else value += c;  // \" \\ など
      escape = false;
      continue;
    }
    if (c == '\\') {
      escape = true;
      continue;
    }
    if (c == '"') {
      out = value;
      return true;
    }
    value += c;
  }
  return false;
}

// JSON文字列として安全に送るためにエスケープする
String jsonEscapeString(const String& src) {
  String out = "";
  out.reserve(src.length() + 16);
  for (size_t i = 0; i < src.length(); ++i) {
    char c = src[i];
    switch (c) {
      case '\"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c; break;
    }
  }
  return out;
}

// CHAT APIでテキストを変換（ずんだもん口調）する
bool requestChatCompletion(const String& inputText, String& outText) {
  // 事前条件（入力テキスト）を確認する
  if (inputText.length() == 0) {
    sp("CHAT input is empty");
    return false;
  }

  // chat/completionsへ送るmessages/payloadを組み立てる
  String url = CHAT_ENDPOINT + "/chat/completions";
  String escapedSystemPrompt = jsonEscapeString(CHAT_SYSTEM_PROMPT);
  String escapedInputText = jsonEscapeString(inputText);
  String chatMessagesJson =
      "["
      "{\"role\":\"developer\",\"content\":\"" + escapedSystemPrompt + "\"},"
      "{\"role\":\"user\",\"content\":\"" + escapedInputText + "\"}"
      "]";
  String payload =
      "{\"model\":\"" + CHAT_MODEL + "\","
      "\"messages\":" + chatMessagesJson + ","
      "\"max_tokens\":2048,"
      "\"temperature\":0.7,"
      "\"stream\":false}";

  // CHAT APIへPOSTしてレスポンスを受け取る
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.useHTTP10(true);
  http.begin(client, url);
  http.setTimeout(30000);
  http.addHeader("Accept", "application/json");
  http.addHeader("Authorization", String("Bearer ") + CHAT_APIKEY);
  http.addHeader("Content-Type", "application/json");
  spf("CHAT url=%s\n", url.c_str());
  spf("CHAT messages=%s\n", chatMessagesJson.c_str());
  spf("CHAT post bytes=%d\n", payload.length());
  int httpCode = http.POST((uint8_t*)payload.c_str(), payload.length());
  String response = http.getString();
  http.end();

  // HTTPエラー時は本文をログして終了する
  spf("CHAT code=%d\n", httpCode);
  if (httpCode != HTTP_CODE_OK) {
    spf("CHAT failed code=%d\n", httpCode);
    spf("CHAT body=%s\n", response.c_str());
    return false;
  }

  // choices[0].message.content を想定（先頭のcontentを簡易抽出）
  if (!extractJsonStringField(response, "content", outText)) {
    spf("CHAT parse failed body=%s\n", response.c_str());
    return false;
  }
  if (outText.length() == 0) {
    spf("CHAT empty content body=%s\n", response.c_str());
    return false;
  }
  return true;
}

// マイク録音（無音で自動停止）してWAVデータを生成する
bool recordVoiceToWavBySilence(uint8_t** outWavBuf, size_t* outWavSize, bool* outUsedPsram) {
  // 出力引数を初期化する
  *outWavBuf = nullptr;
  *outWavSize = 0;
  *outUsedPsram = false;

  // 録音用PCMバッファを確保する（PSRAM優先/必須設定対応）
  const size_t maxSamples = (STT_SAMPLE_RATE * STT_MAX_RECORD_MS) / 1000;
  bool pcmUsedPsram = false;
  int16_t* pcm = (int16_t*)allocateSttBuffer(maxSamples * sizeof(int16_t), &pcmUsedPsram);
  if (!pcm) {
    sp("STT PCM buffer alloc failed (PSRAM required)");
    return false;
  }
  spf("STT PCM buffer: %s\n", pcmUsedPsram ? "PSRAM" : "HEAP");

  // マイク入力前にスピーカー系を確実に停止し、電気的な切替ノイズを減らす
  if (M5.Speaker.isPlaying(0)) M5.Speaker.stop(0);
  M5.Speaker.setVolume(0);
  if (M5.Speaker.isEnabled()) M5.Speaker.end();
  delay(STT_MIC_SETTLE_MS);
  auto micCfg = M5.Mic.config();
  micCfg.sample_rate = STT_SAMPLE_RATE;
  micCfg.stereo = false;
  micCfg.over_sampling = 1;
  micCfg.magnification = STT_MIC_MAGNIFICATION;
  M5.Mic.config(micCfg);
  if (!M5.Mic.begin()) {
    sp("M5.Mic.begin failed");
    freeAudioBuffer((uint8_t*)pcm, pcmUsedPsram);
    M5.Speaker.begin();
    return false;
  }

  // 録音ループ用の状態変数を初期化する
  size_t samplesWritten = 0;
  bool voiceDetected = false;
  uint32_t startMs = millis();
  uint32_t lastVoiceMs = startMs;
  uint32_t lastMeterMs = startMs;
  uint32_t firstVoiceMs = 0;
  int maxLevel = 0;
  int lastLevel = 0;
  bool thresholdLogged = false;
  int16_t block[STT_BLOCK_SAMPLES];

  spf("record start: threshold=%d silence_ms=%d mic_gain=%d\n",
      STT_SILENCE_THRESHOLD, STT_SILENCE_TIMEOUT_MS, STT_MIC_MAGNIFICATION);

      // 録音しつつ、ボタン/無音/時間超過で停止判定する
  while ((millis() - startMs) < STT_MAX_RECORD_MS) {
    M5.update();
    if (M5.BtnA.isPressed()) {
      spf("record stop: button pressed samples=%d duration=%dms\n",
          (int)samplesWritten, (int)(millis() - startMs));
      break;
    }
    if (!M5.Mic.record(block, STT_BLOCK_SAMPLES, STT_SAMPLE_RATE, false)) {
      delay(1);
      continue;
    }

    size_t writable = maxSamples - samplesWritten;
    size_t copySamples = (writable < STT_BLOCK_SAMPLES) ? writable : STT_BLOCK_SAMPLES;
    memcpy(&pcm[samplesWritten], block, copySamples * sizeof(int16_t));
    samplesWritten += copySamples;

    long sumAbs = 0;
    for (size_t i = 0; i < STT_BLOCK_SAMPLES; ++i) sumAbs += abs((int)block[i]);
    int level = (int)(sumAbs / STT_BLOCK_SAMPLES);
    lastLevel = level;
    if (level > maxLevel) maxLevel = level;
    uint32_t now = millis();
    uint32_t elapsed = now - startMs;

    // 録音中のレベルメーター更新（100msごと）、このタイミングでアバターも再描画される点に注意
    if ((now - lastMeterMs) >= 100) {
      drawLevelMeter(level);  // レベルメーター・アバター再描画
      lastMeterMs = now;
    }

    // 録音開始直後はボタン押下/切替ノイズを判定対象外にする
    if (elapsed < STT_RECORD_START_IGNORE_MS) {
      continue;
    }
    if (!thresholdLogged) {
      spf("record threshold: fixed=%d\n", STT_SILENCE_THRESHOLD);
      thresholdLogged = true;
    }

    if (level > STT_SILENCE_THRESHOLD) {
      voiceDetected = true;
      lastVoiceMs = now;
      if (firstVoiceMs == 0) {
        firstVoiceMs = now;
        spf("voice detected: level=%d elapsed=%dms\n", level, (int)(now - startMs));
      }
    }

    if (voiceDetected
      && (now - firstVoiceMs) >= STT_MIN_RECORD_AFTER_VOICE_MS
      && (now - lastVoiceMs) >= STT_SILENCE_TIMEOUT_MS) {
      spf("record stop by silence: level=%d max=%d samples=%d duration=%dms\n",
          level, maxLevel, (int)samplesWritten, (int)(now - startMs));
      break;
    }
    if (!voiceDetected && (now - startMs) >= STT_NO_VOICE_TIMEOUT_MS) {
      spf("record stop: no voice detected (max=%d last=%d)\n", maxLevel, lastLevel);
      break;
    }
    if (samplesWritten >= maxSamples) {
      spf("record stop: max duration reached (max=%d)\n", maxLevel);
      break;
    }
  }

  // 録音終了後にマイク停止・スピーカー再開を行う
  while (M5.Mic.isRecording()) delay(1);
  M5.Mic.end();
  M5.Speaker.begin();

  // 発話が一度も検出されなければ無音扱いで終了する
  if (!voiceDetected) {
    spf("record skipped: silent audio (max=%d last=%d)\n", maxLevel, lastLevel);
    freeAudioBuffer((uint8_t*)pcm, pcmUsedPsram);
    return false;
  }

  if (samplesWritten == 0) {
    sp("record failed: no pcm captured");
    freeAudioBuffer((uint8_t*)pcm, pcmUsedPsram);
    return false;
  }

  // PCMをWAV化するためのバッファを確保する
  uint32_t pcmBytes = samplesWritten * sizeof(int16_t);
  size_t wavSize = WAV_HEADER_SIZE + pcmBytes;
  uint8_t* wavBuf = allocateSttBuffer(wavSize, outUsedPsram);
  if (!wavBuf) {
    sp("STT WAV buffer alloc failed (PSRAM required)");
    freeAudioBuffer((uint8_t*)pcm, pcmUsedPsram);
    return false;
  }
  spf("STT WAV buffer: %s\n", *outUsedPsram ? "PSRAM" : "HEAP");

  // WAVヘッダ付与とPCMコピーを実施する
  writeWavHeader(wavBuf, pcmBytes, STT_SAMPLE_RATE, 1, 16);
  memcpy(wavBuf + WAV_HEADER_SIZE, pcm, pcmBytes);
  freeAudioBuffer((uint8_t*)pcm, pcmUsedPsram);

  // ボタンバッファー削除
  M5.update();
  M5.BtnA.wasReleased();

  // 生成したWAV情報を呼び出し元へ返却する
  *outWavBuf = wavBuf;
  *outWavSize = wavSize;
  spf("record done: wavBytes=%d pcmSamples=%d maxLevel=%d\n", (int)wavSize, (int)samplesWritten, maxLevel);
  return true;
}

// STT APIにWAVを送信して文字起こし結果を取得する
bool transcribeWavWithSakura(const uint8_t* wavBuf, size_t wavSize, String& outText) {
  // multipart/form-dataのヘッダ/フッタ文字列を組み立てる
  const String boundary = "----M5SakuraSttBoundary7MA4YWxkTrZu0gW";
  String partHead =
      "--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"file\"; filename=\"record.wav\"\r\n"
      "Content-Type: audio/wav\r\n\r\n";
  String partTail =
      "\r\n--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"model\"\r\n\r\n" + STT_MODEL + "\r\n"
      "--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"language\"\r\n\r\nja\r\n"
      "--" + boundary + "--\r\n";

  // multipart本文を1つの送信バッファに連結する
  size_t postSize = partHead.length() + wavSize + partTail.length();
  uint8_t* postBody = (uint8_t*)heap_caps_malloc(postSize, MALLOC_CAP_SPIRAM);
  if (!postBody) {
    sp("STT postBody alloc failed (PSRAM)");
    return false;
  }

  size_t offset = 0;
  memcpy(postBody + offset, partHead.c_str(), partHead.length());
  offset += partHead.length();
  memcpy(postBody + offset, wavBuf, wavSize);
  offset += wavSize;
  memcpy(postBody + offset, partTail.c_str(), partTail.length());

  // STT APIへPOSTし、レスポンスJSONを取得する
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.useHTTP10(true);
  http.begin(client, STT_ENDPOINT + "/audio/transcriptions");
  http.setTimeout(65000);
  http.addHeader("Accept", "application/json");
  http.addHeader("Authorization", String("Bearer ") + STT_APIKEY);
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
  spf("STT postBody buffer: PSRAM size=%d\n", (int)postSize);
  spf("STT post bytes=%d\n", (int)postSize);
  int httpCode = http.POST(postBody, postSize);
  heap_caps_free(postBody);

  // HTTPエラー時はレスポンス本文を出して失敗にする
  spf("STT code=%d\n", httpCode);
  String response = http.getString();
  http.end();

  if (httpCode != HTTP_CODE_OK) {
    spf("STT failed code=%d\n", httpCode);
    spf("STT body=%s\n", response.c_str());
    return false;
  }

  // JSONから"text"を取り出して呼び出し元へ返す
  if (!extractJsonStringField(response, "text", outText)) {
    spf("STT parse failed body=%s\n", response.c_str());
    return false;
  }
  String sttTrimmed = outText;
  sttTrimmed.trim();
  if (sttTrimmed.length() == 0) {
    spf("STT empty text. body=%s\n", response.c_str());
    return false;
  }
  outText = sttTrimmed;
  return true;
}

// 録音したWAVをデバッグ用にそのまま再生する（リップシンクなし）
bool playRecordedWavForDebug(const uint8_t* wavBuf, size_t wavSize) {
  if (!wavBuf || wavSize <= WAV_HEADER_SIZE) return false;
  M5.Speaker.setVolume(SPEAKER_VOLUME);
  bool ok = M5.Speaker.playWav(wavBuf, wavSize, 1, 0, true);
  if (!ok) {
    sp("debug playWav failed");
    return false;
  }
  while (M5.Speaker.isPlaying(0)) {
    M5.update();
    delay(5);
  }
  M5.Speaker.setVolume(0);
  return true;
}

// ボタン押下で録音し、無音で停止して文字起こしする
// debugPlayback=true の場合は 録音 -> 再生 -> STT の順に実行する
bool transcribeFromMicBySilence(String& outText, bool debugPlayback, BeforeSttCallback beforeSttCallback = nullptr) {
  // まず無音検知付き録音でWAVを作成する
  uint8_t* wavBuf = nullptr;
  size_t wavSize = 0;
  bool usedPsram = false;
  if (!recordVoiceToWavBySilence(&wavBuf, &wavSize, &usedPsram)) return false;

  // デバッグ有効時は、STT前に録音音声を再生して確認する
  if (debugPlayback) {
    sp("debug playback start");
    playRecordedWavForDebug(wavBuf, wavSize);
    sp("debug playback end");
  }

  // 必要ならSTT直前コールバックを実行する
  if (beforeSttCallback) {
    beforeSttCallback();  // STT前のコールバック
  }

  // STT実行後にバッファを解放して結果を返す
  bool ok = transcribeWavWithSakura(wavBuf, wavSize, outText);
  freeAudioBuffer(wavBuf, usedPsram);
  return ok;
}

// Tiker処理：自動的にキョロキョロする（描画はせず、refreshフラグを立てるのみ）
uint16_t kyoroEyeSelect = 0;  // キョロキョロパターンの選択
static constexpr uint16_t kyoroEyeNum = 4;      // キョロキョロする目の数
const uint8_t kyoroEyePattern[3][kyoroEyeNum] = {  // キョロキョロする目のインデックス番号：目　普通
  { 3,1,2,1 }, { 6,4,5,4 }, { 9,7,8,7 }   // 元気なとき、少し疲れたとき、かなり疲れたとき
};
void startKyorokyoro() {
  if (tickerk.active()) return;
  uint32_t kyoroms = KYORO_TIME_BASE + random(0, KYORO_TIME_RMAX);
  tickerk.attach_ms(kyoroms, tickerKyorokyoro);
}
void tickerKyorokyoro() {
  static uint16_t pat = 0;
  int krrand = random(0,3);// pat++ % kyoroEyeNum
  if (blink) {
    avatar.setBlink("eye", kyoroEyePattern[kyoroEyeSelect][krrand], 0);   // まばたきの目の設定を変更　注意：変更すると即座に反映されるのでrefreshは不要
  } else {
    avatar.changeParts("eye", kyoroEyePattern[kyoroEyeSelect][krrand]);   // 目を変更
    refresh = true;
  }
  // 次回実行間隔をランダムに変える
  if (tickerk.active()) {
    uint32_t kyoroms = KYORO_TIME_BASE + random(0, KYORO_TIME_RMAX);
    tickerk.detach();
    tickerk.attach_ms(kyoroms, tickerKyorokyoro);
  }
}
void stopKyorokyoro() {
  if (!tickerk.active()) return;
  tickerk.detach();
}

// オーディオレベルメーターを描画する（100msごとに呼ばれる。アバターも再描画される）
void drawLevelMeter(int audioLevel) {
  const int meterW = 15;
  const int meterH = 30;
  const int border = 1;
  const int innerW = meterW - border * 2;
  const int innerH = meterH - border * 2;
  const int levelMax = 5000;  // 録音時の実測レンジに合わせた表示上限

  int clamped = audioLevel;
  if (clamped < 0) clamped = 0;
  if (clamped > levelMax) clamped = levelMax;
  int barH = (clamped * innerH) / levelMax;

  uint16_t barColor = TFT_GREEN;
  if (clamped > (levelMax * 7) / 10) barColor = TFT_RED;
  else if (clamped > (levelMax * 4) / 10) barColor = TFT_ORANGE;

  tgauge.fillSprite(0x0020);  // 透明色で全消去
  tgauge.drawRect(0, 0, meterW, meterH, TFT_BLACK);  // 外枠
  if (barH > 0) {
    int y = meterH - border - barH;
    tgauge.fillRect(border, y, innerW, barH, barColor);
  }

  avatar.drawAvatar(); // アバター全体表示
}

// オーディオレベルメーターを消す
void clearLevelMeter() {
  tgauge.fillSprite(0x0020);  // 透明色で全消去
  avatar.drawAvatar(); // アバター全体表示
}

// まばたき停止
void stopBlink() {
  avatar.stopAutoBlink();  // 自動まばたき停止
  avatar.changeParts("eye", blinkOpen);     // 目を戻す
}
// まばたき開始
void startBlink() {
  avatar.setBlink("eye", blinkOpen, 0);   // まばたき用のインデックス番号を設定する
  avatar.startAutoBlink();  // 自動まばたきスタート（タスク実行）
}

// デバッグ: 空きメモリ確認
void debug_free_memory(String str) {
  sp("## "+str);
  spf("heap_caps_get_free_size(MALLOC_CAP_DMA):%d\n", heap_caps_get_free_size(MALLOC_CAP_DMA) );
  spf("heap_caps_get_largest_free_block(MALLOC_CAP_DMA):%d\n", heap_caps_get_largest_free_block(MALLOC_CAP_DMA) );
  spf("heap_caps_get_free_size(MALLOC_CAP_SPIRAM):%d\n", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) );
  spf("heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM):%d\n\n", heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) );
}


// ====================================================================================

// 初期化
void setup() {
  // 初期化（atomic_echoでATOMIC ECHO BASEを使用）
  auto cfg = M5.config();
  cfg.external_speaker.atomic_echo = true;  // ATOMIC ECHO BASE用スピーカーとマイクを有効化
  M5.begin(cfg);
  delay(1000);
  sp("Start!");
  debug_free_memory("Start");

  // WiFi接続
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  sp("Connecting to WiFi...");
  while (WiFi.status() != WL_CONNECTED) {
    M5.update();
    delay(500);
    spn(".");
  }
  sp("");
  spf("WiFi OK: %s", WiFi.localIP().toString().c_str());

  // ログレベルの設定
  // M5.Log.setLogLevel(m5::log_target_serial, ESP_LOG_INFO); // 表示するログレベル ERROR WARN INFO DEBUG VERBOSE
  // M5.Log.setEnableColor(m5::log_target_serial, false);  // ログカラー

  // ディスプレイの設定
  M5.Lcd.init();
  M5.Lcd.setRotation(3);
  M5.Lcd.setColorDepth(16);
  M5.Lcd.fillScreen(TFT_WHITE);

  // アバターの設定
  avatar.usePSRAM(false);   // PSRAMを使わない（128x128ならメインメモリに収まるし速度も速いから）
  String tableNames[]   = { "body", "rhand", "lhand", "eyebrow", "eye", "mouth", "ahiru" };  // 部位名（imgTablesと順番を揃える）
  uint16_t* imgTables[] = { imgTableBody, imgTableRhand, imgTableLhand, imgTableEyebrow, imgTableEye, imgTableMouth, imgTableAhiru };  // 部位ごとにリスト化した表（テーブル）
  avatar.setImageData(imgInfo, tableNames, imgTables, 7); // 画像データを登録する
  avatar.useAntiAliases = true; // アンチエイリアス処理をする
  avatar.mirrorImage = false;   // 左右反転しない（反転はバグあり）
  avatar.setDrawDisplay(&M5.Lcd, 0,0, TFT_WHITE); // アバターの表示先を設定する（出力先, x, y, 背景色）
  avatar.debugtable();
  debug_free_memory("after avater setting");

  // アバターの表示
  defaultAvatarParts(); // アバターのパーツをデフォルトに設定する
  avatar.drawAvatar();  // アバター全体表示

  // ボタン押しながら電源を入れたらデバッグモードに移行
  if (M5.BtnA.isPressed()) {
    // debugmode();  // ToFセンサー距離調整デバッグモード
  }

  // アバターのまばたきの設定と開始
  avatar.setBlink("eye", 1, 0);   // まばたき用のインデックス番号を設定する
  avatar.blink_wait1 = BLINK_TIME_BASE;  // 基本間隔
  avatar.blink_wait2 = BLINK_TIME_RAND;  // ランダム追加 max
  avatar.blink_wait3 = BLINK_TIME_CLOS;  // 閉じてる時間
  avatar.startAutoBlink();  // 自動まばたきスタート（タスク実行）
  blink = true;

  // アバターのリップシンクの設定と開始
  avatar.setLipsync("mouth", 0, 2, 3, 4, 5, 6);   // リップシンク用のインデックス番号を設定する
  avatar.startAutoLipsync();  // リップシンクをスタート（タスク実行）

  // ボイス再生
  // delay(500);
  // playVoiceFromProgmem(0, true);  // 「がんばるのだ」
  // avatar.changeParts("mouth", 0);   // ボイス再生後は念のため口を戻す：口　閉じ　（むふ）
  // avatar.drawAvatar();  // アバター全体表示

  // アバターに挿入するcanvasの設定
  tgauge.setColorDepth(16);
  tgauge.createSprite(15, 30);
  tgauge.fillSprite(0x0020);   // 透明色で塗りつぶし
  avatar.setInsertedLayer(0, OVR_FRONT_AVATER, &tgauge, 1,96);   // ゲージのcanvasをアバターに挿入設定

  // 準備完了
  sp("initialize done!");
  delay(1000);
}


// ====================================================================================

// メイン
void loop() {
  M5.update();

  // ボタンを押ししたら録音開始
  if (M5.BtnA.wasReleased()) {
    String sttText = "";
    String chatText = "";

    // 聞く（録音）
    avatar.changeParts("body", 0);    // 体
    avatar.changeParts("rhand", 6);   // 右腕　マイク
    avatar.changeParts("lhand", 0);   // 左腕
    avatar.changeParts("eyebrow", 0); // 眉毛
    avatar.changeParts("eye", 1);     // 目
    avatar.changeParts("mouth", 0);   // 口
    avatar.changeParts("ahiru", 0);   // アヒル
    avatar.drawAvatar(); // アバター全体表示
    bool res = transcribeFromMicBySilence(sttText, false, []() {  // 【STT 音声認識】
      clearLevelMeter(); // オーディオレベルメーターを消す
      // 考える1（STTで文字起こし）
      stopBlink();  // まばたき停止
      avatar.changeParts("body", 0);    // 体
      avatar.changeParts("rhand", 3);   // 右腕　口元
      avatar.changeParts("lhand", 0);   // 左腕　基本
      avatar.changeParts("eyebrow", 0); // 眉毛
      avatar.changeParts("eye", 0);     // 目　閉じる　（にっこり）
      avatar.changeParts("mouth", 0);   // 口
      avatar.changeParts("ahiru", 0);   // アヒル
      avatar.drawAvatar(); // アバター全体表示
    });
    spf("STT text=%s\n", sttText.c_str());
    if (res) {
      // 考える2（CHATでずんだもん語変換）
      avatar.changeParts("body", 0);    // 体
      avatar.changeParts("rhand", 1);   // 右腕　腰
      avatar.changeParts("lhand", 5);   // 左腕　考える
      avatar.changeParts("eyebrow", 3); // 眉　困り眉1
      avatar.changeParts("eye", 13);     // 目　ジト目
      avatar.changeParts("mouth", 0);   // 口
      avatar.changeParts("ahiru", 0);   // アヒル
      avatar.drawAvatar(); // アバター全体表示
      if (requestChatCompletion(sttText, chatText)) {   // 【CHAT ずんだもん語変換】
        spf("CHAT text=%s\n", chatText.c_str());
        String ttsText = chatText;
        if (ttsText.length() > CHAT_TTS_MAX_CHARS) {
          ttsText = ttsText.substring(0, CHAT_TTS_MAX_CHARS);
        }
        if (ttsText.length() != chatText.length()) {
          spf("CHAT text truncated for TTS: %d -> %d\n", chatText.length(), ttsText.length());
        }
        // 喋る（TTSで音声合成して再生）
        playVoiceFromText(ttsText.c_str(), true, []() { // 【TTS 音声合成】+ WAV再生
          startBlink();  // まばたき開始
          avatar.changeParts("body", 0);    // 体
          avatar.changeParts("rhand", 2);   // 右腕　手を挙げる
          avatar.changeParts("lhand", 2);   // 左腕　手を挙げる
          avatar.changeParts("eyebrow", 0); // 眉毛
          avatar.changeParts("eye", 1);     // 目
          avatar.changeParts("mouth", 0);   // 口
          avatar.changeParts("ahiru", 0);   // アヒル
          avatar.drawAvatar(); // アバター全体表示
        });
      } else {
        sp("CHAT failed");
      }
    } else {
      sp("STT failed");
    }

    // 終了
    defaultAvatarParts();   // アバターのパーツをデフォルトに設定する
    avatar.drawAvatar();  // アバター全体表示
  }

  // ボタンを押ししたら喋る
  if (0 && M5.BtnA.wasReleased()) {
    // ボイス再生（URLからストリーミング）
    avatar.changeParts("rhand", 2);   // 右腕　上げ
    avatar.changeParts("lhand", 2);   // 左腕　上げ
    avatar.drawAvatar();  // アバター全体表示
    // playVoiceFromProgmem(1, true);  // 「ちょっと休憩するのだ」
    // playVoiceFromUrl("https://xxx/xxx.wav", true); // 24kHz, 16bit, mono
    playVoiceFromText("ボクはずんだもんなのだ。さくらのAIエンジンで喋ってるのだ。", true);
    avatar.changeParts("mouth", 0);   // ボイス再生後は念のため口を戻す：口　閉じ　（むふ）
    avatar.changeParts("rhand", 0);   // 右腕　基本
    avatar.changeParts("lhand", 0);   // 左腕　基本
    avatar.drawAvatar();  // アバター全体表示
  }

  // キョロキョロ（kyoro変更時に1回のみ行う処理）
  static bool lastkyoro = false;
  if (kyoro != lastkyoro) {
    if (kyoro) {
      startKyorokyoro();  // キョロキョロ開始
    } else {
      stopKyorokyoro();  // キョロキョロ終了
      avatar.changeParts("eye", blinkOpen);     // 目を戻す
      refresh = true;
    }
    lastkyoro = kyoro;
  }

  // 一旦ここでアバターを表示する
  if (refresh) {
    avatar.drawAvatar(); // アバター全体表示
    refresh = false;
  }

  delay(10);
}

