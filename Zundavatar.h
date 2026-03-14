/*
  Zundavatar.h
  ズンダチャン　アバターCLASS

  Copyright (c) 2024 Kaz  (https://akibabara.com/blog/)
  Released under the MIT license.
  see https://opensource.org/licenses/MIT
*/
#pragma once
#include <M5Unified.h>
//#include <M5GFX.h>

// デバッグに便利なマクロ定義 -- M5.Log置き換えver --------
#define sp(x) M5.Log.println(String(x).c_str())
#define spn(x) M5.Log.print(String(x).c_str())
#define spp(k,v) M5.Log.println(String(String(k)+"="+String(v)).c_str())
#define spf(fmt, ...) M5.Log.printf(fmt, __VA_ARGS__)

namespace zundavatar {

// 定数
#define OVR_NONE 0          // レイヤーの挿入なし
#define OVR_BACKGROUND 1    // 最背面に挿入（重い）
#define OVR_BACK_AVATER 2   // アバターの背面に挿入（軽い）変形モード使わないならこっちでいい
#define OVR_FRONT_AVATER 3  // アバターの前面に挿入（軽い）変形モード使わないならこっちでいい
#define OVR_FOREGROUND 4    // 最前面に挿入（重い）

// 定数
static constexpr uint16_t tableNumZundavatar = 10;  // 登録可能な部位の種類の上限

// 構造体など
struct ImageInfo {  // 画像データの構造体
  const unsigned short* data;
  const uint16_t width;
  const uint16_t height;
  const uint16_t size;
  const uint16_t posX;
  const uint16_t posY;
  const unsigned short transparent;
};
struct XYaddress { int16_t x; int16_t y; };
struct XYWHaddress { int16_t x; int16_t y; uint16_t w; uint16_t h; };
static constexpr uint16_t InsertLayerNum = 10;      // 挿入可能なレイヤーの数
struct InsertLayer {  // 挿入するレイヤーの構造体
  uint16_t position;   // 挿入位置 1=背面 2=前面 0=しない
  M5Canvas* canvas;
  int16_t x, y, w, h;
};

//
// ==== ズンダチャン アバター生成CLASS =====================
//
class Zundavatar {
public:
  // メンバ変数 public:
  const ImageInfo* _imgInfo;  // 画像データと情報 {data, x, y, size, w, h, transparent}
  uint16_t tableNum = tableNumZundavatar;   // 登録可能な部位の種類の上限
  String _tableNames[tableNumZundavatar];   // 部位の名前一覧
  uint16_t* _imgTables[tableNumZundavatar]; // その部位に対応する配列へのポインタ

  /* テーブル変数の構成
  * _imgTables[ テーブル番号(tbl) ] = { 画像番号(no), 画像番号(no), 画像番号(no) }
  *                                    [0]           [1]          [2] ←インデックス番号(idx)
  * _imgInfo[ 画像番号(no) ] = { data, x, y, size, w, h, transparent }  個々の画像
  */

  // 表示するパーツに関する情報、動かすパーツの指定など
  int16_t items[tableNumZundavatar];    // 部位ごとの表示させるインデックス番号
  M5Canvas canvas_body, canvas_body2;   // 一時利用するキャンバス
  InsertLayer InsertLayers[InsertLayerNum]; // 挿入するレイヤー

  bool expandCanvas = false;                        // 体のキャンバスサイズを変更する
  XYWHaddress expandCanvasInfo = { 0, 0, 0, 0 };    // 体のキャンバスサイズの変更内容
  bool usePsram = true;         // PSRAMを使う
  bool useAntiAliases = false;  // アンチエイリアスを使う
  bool mirrorImage = false;     // 左右反転
  uint16_t rotateMirrorOff = 0; // 左右反転なし時のsetRotation値
  uint16_t rotateMirrorOn = 6;  // 左右反転あり時のsetRotation値
  unsigned short transparentDefault = 0x0000; // デフォルトの透明色（実際は0x2000）
  float scaleBodyCanvasX = 1.0; // アバターの表示スケールX
  float scaleBodyCanvasY = 1.0; // アバターの表示スケールY

  LovyanGFX* drawDisplay = nullptr;     // 出力先のキャンバスまたはディスプレイ
  uint16_t drawX = 0, drawY = 0;        // 出力先の座標
  unsigned short drawBackgroundColor = 0x0000;  // 出力時の透明色
  String defaultBaseBodyName = "body";  // 基準となる体のテーブル名

  // 状態
  bool nowDrawing = false;        // 描画中はtrueになる

  // 自動まばたき関連
  String autoBlinkName = "";      // 自動まばたきのテーブル名
  int16_t autoBlinkIdx_open = 0;  // 自動まばたき：目のインデックス番号 OPEN
  int16_t autoBlinkIdx_close = 0; // 自動まばたき：目のインデックス番号 CLOSE
  bool autoBlink = false;         // 自動まばたきの有効化
  uint16_t blink_wait1 = 2000;    // まばたきの間隔：固定分
  uint16_t blink_wait2 = 1000;    // まばたきの間隔：ランダム分
  uint16_t blink_wait3 = 150;     // まばたきの長さ

  // リップシンク関連
  String autoLipsyncName = "";    // リップシンクのテーブル名
  int16_t autoLipsyncIdxs[6];     // リップシンク：口のインデックス番号（ん,あ,い,う,え,お）
  bool autoLipsync = false;       // リップシンクの有効化
  uint8_t autoLipsyncNowVowel = 0;// 現在表示中の母音
  uint16_t lip_wait = 150;        // 口を開けている時間
  uint16_t lip_waittmp = 0;       // 口を開けている時間（1回限り）

  // メンバ関数 public:
  Zundavatar(bool* lcdBusyPointer);
  ~Zundavatar() = default;

  XYaddress img_get_offset(ImageInfo base, ImageInfo target);  // 2つの画像の座標を元にオフセット位置を求める
  uint16_t name2table(String name);   // 部位名からテーブル番号を求める
  uint16_t nameidx2no(String name, int16_t idxOrDefault=-1);    // 部位名・インデックス番号から画像番号を求める（インデックス番号省略時はitems[]を参照）
  void setImageData(const ImageInfo* imgInfo, String* tableNames, uint16_t* imgTables[], size_t len); // 画像データとテーブル情報を登録する
  void changeParts(String name, int16_t idx);   // 指定部位に表示する画像を登録する
  void usePSRAM(bool psram);        // PSRAMを使う
  void debugtable();
  bool lockDisplay(uint32_t timeout=1000);   // Displayの排他処理　ロック開始
  void unlockDisplay();   // Displayの排他処理　ロック解除
  void setInsertedLayer(uint16_t no, uint16_t position, M5Canvas* canvas, int16_t x, int16_t y);   // アバターに挿入するレイヤーの設定
  void unsetInsertedLayer(uint16_t no, uint16_t position);   // アバターに挿入するレイヤーの削除
  void changeInsertedLayerPosition(uint16_t no, uint16_t position); // アバターに挿入するレイヤーの変更：階層
  void changeInsertedLayerXY(uint16_t no, int16_t x, int16_t y);  // アバターに挿入するレイヤーの変更：座標

  void _makeAvater(   LovyanGFX* dst, int16_t x, int16_t y, unsigned short bgColor, XYWHaddress* trim=nullptr);   // アバターを合成してキャンバスに出力する
  void makeAvater(    LovyanGFX* dst, int16_t x, int16_t y) { _makeAvater(dst, x, y, transparentDefault, nullptr); };
  void makeAvater(    LovyanGFX* dst, int16_t x, int16_t y, unsigned short bgColor) { _makeAvater(dst, x, y, bgColor, nullptr); };
  void makeAvaterTrim(LovyanGFX* dst, int16_t x, int16_t y, unsigned short bgColor, XYWHaddress* trim) { _makeAvater(dst, x, y, bgColor, trim); };

  // メンバ関数
  void setDrawDisplay(LovyanGFX* dst, uint16_t x, uint16_t y, unsigned short bgColor);  // アバターの出力先を設定する
  void changeDrawPosition(uint16_t x, uint16_t y);  // アバターの出力先の座標を変更する
  void drawAvatar(bool drawWait=true);  // アバターを生成して出力する（全体表示を行う）
  void drawAvatarTrim(int16_t x, int16_t y, uint16_t w, uint16_t h, bool drawWait=true);  // アバターを生成して指定した範囲だけを出力する（一部分だけの書き換え用途）
  void setEnpandCanvas(int16_t x, int16_t y, uint16_t w, uint16_t h);   // 描画エリアの拡張
  void clearEnpandCanvas();   // 描画エリアの拡張をやめる
  void setBlink(String name, int16_t idxOpen, int16_t idxClose);  // 自動まばたきの設定
  void startAutoBlink();  // 自動まばたきを開始する
  void stopAutoBlink();   // 自動まばたきを終了する
  void setLipsync(String name, int16_t nn, int16_t aa, int16_t ii, int16_t uu, int16_t ee, int16_t oo);  // リップシンクの設定
  void setLipsyncVowel(uint8_t vowel, int16_t lipWaitTmp=0);  // リップシンクの母音と自動的に口を閉じるまでの時間を設定する（設定すると即反映される）
  void startAutoLipsync();  // リップシンクを開始する
  void stopAutoLipsync();   // リップシンクを終了する
  void createExpression();  // 表情セットを作成する
  void apllyExpression();   // 表情セットを反映する

private:
  M5Canvas tmpcanvas;   // 一時利用するキャンバス
  bool* _lcdBusyPtr;    // LCDをクラス間で排他処理するためのフラグ　へのポインター

};

// from M5Stack-Avatar (https://github.com/meganetaaan/m5stack-avatar)
class DriveContext {
 private:
  Zundavatar *avatar;
 public:
  DriveContext() = delete;
  explicit DriveContext(Zundavatar *avatar);
  ~DriveContext() = default;
  DriveContext(const DriveContext &other) = delete;
  DriveContext &operator=(const DriveContext &other) = delete;
  Zundavatar *getZundavatar();
};

} // namespace zundavatar
