/*
  Zundavatar.cpp
  ズンダチャン　アバターCLASS

  Copyright (c) 2024 Kaz  (https://akibabara.com/blog/)
  Released under the MIT license.
  see https://opensource.org/licenses/MIT
*/
#include "Zundavatar.h"
namespace zundavatar {

DriveContext::DriveContext(Zundavatar *avatar) : avatar{avatar} {}
Zundavatar *DriveContext::getZundavatar() { return avatar; }

//
// ==== ズンダチャン アバターCLASS =====================
//

// コンストラクタ
Zundavatar::Zundavatar(bool* lcdBusyPointer) {
  _lcdBusyPtr = lcdBusyPointer;
  sp("Zundavatar::Zundavatar");
  for (int i=0; i<tableNum; i++) {
    _tableNames[i] = "";
    _imgTables[i] = nullptr;
    items[i] = -1;
  }
  for (int i=0; i<InsertLayerNum; i++) {
    InsertLayers[i] = { 0, nullptr, 0, 0, 0, 0 };
    //InsertLayers[i].position = 0;
  }
}

// 2つの画像の座標を元にオフセット位置を求める
XYaddress Zundavatar::img_get_offset(ImageInfo base, ImageInfo target) {
  XYaddress xy;
  xy.x = target.posX - base.posX;
  xy.y = target.posY - base.posY;
  return xy;
}

// 部位名からテーブル番号を求める
uint16_t Zundavatar::name2table(String name) {
  uint16_t tbl = -1;
  for (int i=0; i<tableNum; i++) {
    if (_tableNames[i] == "") break;
    if (name == _tableNames[i]) {
    tbl = i;
    break;
    }
  }
  return tbl;
}

// 部位名・インデックス番号から画像番号を求める（インデックス番号省略時はitems[]を参照）
uint16_t Zundavatar::nameidx2no(String name, int16_t idxOrDefault) {
  uint16_t tbl, idx, no = -1;
  if (name != "") {
    tbl = name2table(name);
    if (tbl != -1) {
      idx = (idxOrDefault >= 0) ? idxOrDefault : items[tbl];
      no = _imgTables[tbl][idx];
//spf("%s %d %d (%d) %d\n", name,tbl,idx,idxOrDefault, no);
    }
  }
  return no;
}

// 画像データとテーブル情報を登録する
void Zundavatar::setImageData(const ImageInfo* imgInfo, String* tableNames, uint16_t* imgTables[], size_t len) {
  _imgInfo = imgInfo;
  transparentDefault = _imgInfo[0].transparent; // 画像番号0の透明色をデフォルトの透明色とする
  for (int i=0; i<len; i++) {
    if (i >= tableNum) break;
    _tableNames[i] = tableNames[i];
    spf("setTableData: %s\n", _tableNames[i]);
    _imgTables[i] = imgTables[i];
  }
}

// 指定部位に表示する画像を登録する
void Zundavatar::changeParts(String name, int16_t idx) {
  uint16_t tbl = name2table(name);
  if (tbl == -1) return;
  items[tbl] = idx;
  // 自動まばたき実行中はまばたき設定も同時に変える
  //if (autoBlink && name == autoBlinkName) {
  //  autoBlinkIdx_open = idx;
  //}
}

// PSRAMを使う
void Zundavatar::usePSRAM(bool psram) {
  usePsram = psram;
}

void Zundavatar::debugtable() {
  for (int i=0; i<tableNum; i++) {
    spf("_tableNames[%d]=%s\n",i,_tableNames[i]);
  }
}

// Displayの排他処理　ロック開始
bool Zundavatar::lockDisplay(uint32_t timeout) {
  if (_lcdBusyPtr == nullptr) return true;
  bool res = false;
  uint32_t tm = millis() + timeout;
  while (millis() < tm) {
    if (! *_lcdBusyPtr) {
      res = true;
      *_lcdBusyPtr = true;
      break;
    }
    delay(5);
  }
  return res;
}

// Displayの排他処理　ロック解除
void Zundavatar::unlockDisplay() {
  if (_lcdBusyPtr == nullptr) return;
  *_lcdBusyPtr = false;
}

// アバターに挿入するレイヤーの追加
void Zundavatar::setInsertedLayer(uint16_t no, uint16_t position, M5Canvas* canvas, int16_t x, int16_t y) {
  if (no >= 0 && no < InsertLayerNum && canvas != nullptr) {
    InsertLayers[no] = { position, canvas, x, y, (int16_t)canvas->width(), (int16_t)canvas->height() };
  }
}

// アバターに挿入するレイヤーの削除
void Zundavatar::unsetInsertedLayer(uint16_t no, uint16_t position) {
  if (no >= 0 && no < InsertLayerNum) {
    InsertLayers[no] = { 0, nullptr, 0, 0, 0, 0 };
  }
}

// アバターに挿入するレイヤーの変更：階層
void Zundavatar::changeInsertedLayerPosition(uint16_t no, uint16_t position) {
  if (no >= 0 && no < InsertLayerNum) {
    InsertLayers[no].position = position;
  }
}

// アバターに挿入するレイヤーの変更：座標
void Zundavatar::changeInsertedLayerXY(uint16_t no, int16_t x, int16_t y) {
  if (no >= 0 && no < InsertLayerNum) {
    InsertLayers[no].x = x;
    InsertLayers[no].y = y;
  }
}

// アバターを合成してキャンバスに出力する
void Zundavatar::_makeAvater(LovyanGFX* dst, int16_t x, int16_t y, unsigned short bgColor, XYWHaddress* trim) {
  int i;
  int16_t tbl, idx, no;
  uint16_t idxno, bdw, bdh;
  unsigned short transparent, transparentLE;
  XYaddress ofs, out = {x, y}; 

  // 基準となる体のテーブル番号・インデックス番号・画像番号を求める
  int16_t body_no = nameidx2no("body"); //（第2引数省略時はitems[]を参照）
  if (body_no == -1) return;

  // 透明色を求める
  transparent = _imgInfo[body_no].transparent;
  transparentLE = (transparent & 0xFF) << 8 | (transparent & 0xFF00) >> 8;

  // レイヤー挿入前処理
  bool ovr_bg = false;
  bool ovr_fg = false;
  for (i=0; i<InsertLayerNum; i++) {
    if (InsertLayers[i].position == OVR_BACKGROUND && InsertLayers[i].canvas != nullptr) ovr_bg = true;
    if (InsertLayers[i].position == OVR_FOREGROUND && InsertLayers[i].canvas != nullptr) ovr_fg = true;
  }

  // キャンバスを作成する
  canvas_body.setPsram(usePsram);
  canvas_body.setColorDepth(16);
  canvas_body.setRotation(mirrorImage ? rotateMirrorOn : rotateMirrorOff);
  if (trim == nullptr) {
    bdw = _imgInfo[body_no].width;
    bdh = _imgInfo[body_no].height;
    if (expandCanvas) {  // キャンバスのサイズを拡張した場合
      bdw += expandCanvasInfo.w;
      bdh += expandCanvasInfo.h;
    }
  } else {  // トリムモードの場合は指定されたサイズのキャンバスにする
    bdw = trim->w;
    bdh = trim->h;
  }
  canvas_body.createSprite(bdw, bdh);
  canvas_body.startWrite();
  canvas_body.fillSprite(ovr_bg ? transparent : bgColor);

  // アバターの背面にレイヤーを挿入する
  for (i=0; i<InsertLayerNum; i++) {
    if (InsertLayers[i].position == OVR_BACK_AVATER && InsertLayers[i].canvas != nullptr) {
      ofs.x = InsertLayers[i].x - ((trim == nullptr) ? 0 : trim->x);
      ofs.y = InsertLayers[i].y - ((trim == nullptr) ? 0 : trim->y);
      InsertLayers[i].canvas->pushSprite(&canvas_body, ofs.x, ofs.y, transparent);
    }
  }

  // 部位順に画像を重ねていく
  for (tbl=0; tbl<tableNum; tbl++) {
    if (_tableNames[tbl] == "") break;
    idx = items[tbl];
    if (idx == -1) continue;

    // 配置先座標のオフセット値を求める
    no = _imgTables[tbl][idx];
    ofs = img_get_offset(_imgInfo[body_no], _imgInfo[no]);
    if (trim == nullptr) {
      if (expandCanvas) {  // キャンバスのサイズを拡張した場合
        ofs.x += expandCanvasInfo.x;
        ofs.y += expandCanvasInfo.y;
      }
    } else {  // トリムモードの場合は出力先座標を調整する
      ofs.x -= trim->x;
      ofs.y -= trim->y;
    }

    // キャンバスに画像をコピーする
    canvas_body.pushImage(ofs.x, ofs.y, _imgInfo[no].width, _imgInfo[no].height, _imgInfo[no].data, transparentLE);
  }

  // アバターの前面にレイヤーを挿入する
  for (i=0; i<InsertLayerNum; i++) {
    if (InsertLayers[i].position == OVR_FRONT_AVATER && InsertLayers[i].canvas != nullptr) {
      ofs.x = InsertLayers[i].x - ((trim == nullptr) ? 0 : trim->x);
      ofs.y = InsertLayers[i].y - ((trim == nullptr) ? 0 : trim->y);
      InsertLayers[i].canvas->pushSprite(&canvas_body, ofs.x, ofs.y, transparent);
    }
  }

  // 出力
  if (trim != nullptr) {  // トリムモードの場合は出力先座標を調整する
    out.x += trim->x;
    out.y += trim->y;
    if (expandCanvas) {  // キャンバスのサイズを拡張した場合
      out.x += expandCanvasInfo.x;
      out.y += expandCanvasInfo.y;
    }
  }
  if (scaleBodyCanvasX == 1.0 && scaleBodyCanvasY == 1.0 && !ovr_bg && !ovr_fg) {
    // 変形なし・レイヤー挿入なしの場合はそのまま貼り付け
    if (lockDisplay()) canvas_body.pushSprite(dst, out.x, out.y, transparent);
    unlockDisplay();
    delay(1);
  } else {
    // 変形あり or レイヤー挿入ありの場合は作業用のキャンバスを作る canvas_body --> canvas_body2 --> dst
    canvas_body2.setPsram(usePsram);
    canvas_body2.setColorDepth(16);
    canvas_body2.createSprite(bdw, bdh);
    canvas_body2.startWrite();
    canvas_body2.fillSprite(bgColor);
    uint16_t x2 = canvas_body.width() / 2.0;
    uint16_t y2 = canvas_body.height();
    // 背景挿入
    for (i=0; i<InsertLayerNum; i++) {
      if (InsertLayers[i].position == OVR_BACKGROUND && InsertLayers[i].canvas != nullptr) {
        ofs.x = InsertLayers[i].x - ((trim == nullptr) ? 0 : trim->x);
        ofs.y = InsertLayers[i].y - ((trim == nullptr) ? 0 : trim->y);
        InsertLayers[i].canvas->pushSprite(&canvas_body2, ofs.x, ofs.y, transparent);
      }
    }
    // アバター挿入
    canvas_body.setPivot(x2, y2);  // 下/中央が基準点
    if (useAntiAliases) {
      canvas_body.pushRotateZoomWithAA(&canvas_body2, x2,y2, 0, scaleBodyCanvasX, scaleBodyCanvasY, transparent);
    } else {
      canvas_body.pushRotateZoom(&canvas_body2, x2,y2, 0, scaleBodyCanvasX, scaleBodyCanvasY, transparent);
    }
    // 前景挿入
    for (i=0; i<InsertLayerNum; i++) {
      if (InsertLayers[i].position == OVR_FOREGROUND && InsertLayers[i].canvas != nullptr) {
        ofs.x = InsertLayers[i].x - ((trim == nullptr) ? 0 : trim->x);
        ofs.y = InsertLayers[i].y - ((trim == nullptr) ? 0 : trim->y);
        InsertLayers[i].canvas->pushSprite(&canvas_body2, ofs.x, ofs.y, transparent);
      }
    }
    // 出力
    delay(1);
    if (lockDisplay()) canvas_body2.pushSprite(dst, out.x, out.y, transparent);
    unlockDisplay();
    delay(1);
    canvas_body2.endWrite();
    canvas_body2.deleteSprite();
  }

  canvas_body.endWrite();
  canvas_body.deleteSprite();
}

// アバターの出力先を設定する
void Zundavatar::setDrawDisplay(LovyanGFX* display, uint16_t x, uint16_t y, unsigned short bgColor) {
  //LovyanGFX* display, MakeAvater* avatar, uint16_t x, uint16_t y, const unsigned short* bgImgData
  drawDisplay = display;
  drawX = x;
  drawY = y;
  drawBackgroundColor = bgColor;
  if (usePsram && heap_caps_get_free_size(MALLOC_CAP_SPIRAM) == 0) {
    usePsram = false;  // PSRAMが無い場合はPSRAMを使わない
  }
}

// アバターの出力先の座標を変更する
void Zundavatar::changeDrawPosition(uint16_t x, uint16_t y) {
  drawX = x;
  drawY = y;
}

// アバターを生成して出力する（全体表示を行う）
void Zundavatar::drawAvatar(bool drawWait) {
  int loop = 0;
  while (nowDrawing && drawWait) { // 排他処理：描画中なら待機する。ただし100msまで
    delay(1);
    if (++loop > 100) break;
  }
  if (!nowDrawing) {
    nowDrawing = true;
    makeAvater(drawDisplay, drawX, drawY, drawBackgroundColor);
    nowDrawing = false;
  }
}

// アバターを生成して指定した範囲だけを出力する（一部分だけの書き換え用途）
void Zundavatar::drawAvatarTrim(int16_t x, int16_t y, uint16_t w, uint16_t h, bool drawWait) {
  int loop = 0;
  while (nowDrawing && drawWait) { // 排他処理：描画中なら待機する。ただし100msまで
    delay(1);
    if (++loop > 100) break;
  }
  if (!nowDrawing) {
    XYWHaddress trim = { x, y, w, h };
    nowDrawing = true;
    makeAvaterTrim(drawDisplay, drawX, drawY, drawBackgroundColor, &trim);
    nowDrawing = false;
  }
}

// 描画エリアの拡張
void Zundavatar::setEnpandCanvas(int16_t x, int16_t y, uint16_t w, uint16_t h) {
  expandCanvas = true;
  expandCanvasInfo = { x, y, w, h };
}

// 描画エリアの拡張をやめる
void Zundavatar::clearEnpandCanvas() {
  expandCanvas = false;
  expandCanvasInfo = { 0, 0, 0, 0 };
}

// 自動まばたきの設定
void Zundavatar::setBlink(String name, int16_t idxOpen, int16_t idxClose) {
  autoBlinkName = name;
  autoBlinkIdx_open = idxOpen;
  autoBlinkIdx_close = idxClose;
  //changeParts(name, idxOpen);
}

// タスク処理：自動まばたき
void taskBlinkLoop(void *args) {
  DriveContext *ctx = reinterpret_cast<DriveContext *>(args);
  Zundavatar *avatar = ctx->getZundavatar();
  unsigned long nextms = 0;
  int16_t body_no, eye_tbl, eye_no, x, y, nowidx;
  uint16_t w, h, px, pz;
  XYaddress ofs;

  if (avatar->name2table(avatar->autoBlinkName) != -1) {
    // 描画範囲を決める
    eye_no = avatar->nameidx2no(avatar->autoBlinkName, avatar->autoBlinkIdx_open);
    body_no = avatar->nameidx2no(avatar->defaultBaseBodyName); // 基準となる体のテーブル番号・インデックス番号・画像番号を求める
    if (body_no == -1) return;
    ofs = avatar->img_get_offset(avatar->_imgInfo[body_no], avatar->_imgInfo[eye_no]);
    x = ofs.x;
    y = ofs.y;
    w = avatar->_imgInfo[eye_no].width;
    h = avatar->_imgInfo[eye_no].height;

    // 自動まばたき開始
    while (avatar->autoBlink) {
      // 左右反転時は座標を調整する
      if (avatar->mirrorImage) {
        px = avatar->_imgInfo[eye_no].posX - avatar->_imgInfo[body_no].posX;
        pz = avatar->_imgInfo[body_no].width - avatar->_imgInfo[eye_no].width - px;
        x = x - pz - px;  // 間違いのはずがなぜか正しく動く??よくわからんがヨシ！
      }

      // 閉じる
      nowidx = avatar->autoBlinkIdx_open;
      nextms = millis() + avatar->blink_wait1 + random(0, avatar->blink_wait2);
      while (nextms > millis()) {
        if (!avatar->autoBlink) break;
        // 待機中に自動まばたきの目が変わったら即座に反映させる
        if (nowidx != avatar->autoBlinkIdx_open) {
          avatar->changeParts(avatar->autoBlinkName, avatar->autoBlinkIdx_open); // 開く
          avatar->drawAvatarTrim(x, y, w, h, true);  // 変更部分だけを描画する、排他処理あり
          nowidx = avatar->autoBlinkIdx_open;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
      }
      if (!avatar->autoBlink) break;
      avatar->changeParts(avatar->autoBlinkName, avatar->autoBlinkIdx_close); // 閉じる
      avatar->drawAvatarTrim(x, y, w, h, true);  // 変更部分だけを描画する、排他処理あり
      // 開く
      nextms = millis() + avatar->blink_wait3;
      while (nextms > millis()) {
        if (!avatar->autoBlink) break;
        vTaskDelay(pdMS_TO_TICKS(5));
      }
      if (!avatar->autoBlink) break;
      avatar->changeParts(avatar->autoBlinkName, avatar->autoBlinkIdx_open); // 開く
      avatar->drawAvatarTrim(x, y, w, h, true);  // 変更部分だけを描画する、排他処理あり
    }
  }
  vTaskDelete(NULL);
}

// 自動まばたきを開始する
void Zundavatar::startAutoBlink() {
  DriveContext *ctx = new DriveContext(this);
  if (!autoBlink && name2table(autoBlinkName) != -1) {
    // タスクを作成する
    autoBlink = true;
    xTaskCreateUniversal(
      taskBlinkLoop,  // Function to implement the task
      "taskBlinkLoop",// Name of the task
      2048,           // Stack size in words
      ctx,            // Task input parameter
      3,              // Priority of the task
      NULL,           // Task handle.
      CONFIG_ARDUINO_RUNNING_CORE);
  }
}

// 自動まばたきを終了する
void Zundavatar::stopAutoBlink() {
  if (autoBlink) {
    // タスクを削除する（実際はタスク内で処理）
    autoBlink = false;
  }
}

// リップシンクの設定
void Zundavatar::setLipsync(String name, int16_t nn, int16_t aa, int16_t ii, int16_t uu, int16_t ee, int16_t oo) {
  if (name2table(name) != -1) {
    autoLipsyncName = name;
    autoLipsyncIdxs[0] = nn;
    autoLipsyncIdxs[1] = aa;
    autoLipsyncIdxs[2] = ii;
    autoLipsyncIdxs[3] = uu;
    autoLipsyncIdxs[4] = ee;
    autoLipsyncIdxs[5] = oo;
    autoLipsyncNowVowel = 0;
    //changeParts(name, nn);
  }
}

// リップシンクの母音と自動的に口を閉じるまでの時間を設定する（設定すると即反映される）
void Zundavatar::setLipsyncVowel(uint8_t vowel, int16_t lipWaitTmp) {
  autoLipsyncNowVowel = vowel;
  if (lipWaitTmp > 0) lip_waittmp = lipWaitTmp;
}

// タスク処理：リップシンク
void taskLipsyncLoop(void *args) {
  DriveContext *ctx = reinterpret_cast<DriveContext *>(args);
  Zundavatar *avatar = ctx->getZundavatar();
  unsigned long nextms = 0;
  int16_t body_no, mouth_tbl, mouth_no, x, y, idx;
  uint16_t w, h, px, pz;
  XYaddress ofs;
  uint8_t lastLip = 0;

  if (avatar->name2table(avatar->autoLipsyncName) != -1) {
    // 描画範囲を決める
    mouth_no = avatar->nameidx2no(avatar->autoLipsyncName, avatar->autoLipsyncIdxs[0]);
    body_no = avatar->nameidx2no(avatar->defaultBaseBodyName); // 基準となる体のテーブル番号・インデックス番号・画像番号を求める
    if (body_no == -1) return;
    ofs = avatar->img_get_offset(avatar->_imgInfo[body_no], avatar->_imgInfo[mouth_no]);
    x = ofs.x;
    y = ofs.y;
    w = avatar->_imgInfo[mouth_no].width;
    h = avatar->_imgInfo[mouth_no].height;

    // リップシンク開始
    while (avatar->autoLipsync) {
      // 左右反転時は座標を調整する
      if (avatar->mirrorImage) {
        px = avatar->_imgInfo[mouth_no].posX - avatar->_imgInfo[body_no].posX;
        pz = avatar->_imgInfo[body_no].width - avatar->_imgInfo[mouth_no].width - px;
        x = x - pz - px;  // 間違いのはずがなぜか正しく動く??よくわからんがヨシ！
      }
      if (avatar->autoLipsyncNowVowel != lastLip) {
        // 指定した口の形を表示する
        if (avatar->autoLipsyncNowVowel >= 0 && avatar->autoLipsyncNowVowel <= 5) {
          idx = avatar->autoLipsyncIdxs[avatar->autoLipsyncNowVowel];
        }
        else idx = -1;
        lastLip = avatar->autoLipsyncNowVowel;
        avatar->changeParts(avatar->autoLipsyncName, idx);
        avatar->drawAvatarTrim(x, y, w, h, true);  // 変更部分だけを描画する、排他処理あり
        if (avatar->lip_waittmp > 0) {
          nextms = millis() + avatar->lip_waittmp;
          avatar->lip_waittmp = 0;
        } else {
          nextms = millis() + avatar->lip_wait;
        }
      } else if (nextms != 0 && nextms < millis()) {
        // 一定時間経過後に自動的に口を閉じる
        avatar->autoLipsyncNowVowel = 0;
        idx = avatar->autoLipsyncIdxs[0];
        lastLip = 0;
        avatar->changeParts(avatar->autoLipsyncName, idx);
        avatar->drawAvatarTrim(x, y, w, h, true);  // 変更部分だけを描画する、排他処理あり
        nextms = 0;
      }
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
  vTaskDelete(NULL);
}

// リップシンクを開始する
void Zundavatar::startAutoLipsync() {
  DriveContext *ctx = new DriveContext(this);
  if (!autoLipsync && name2table(autoLipsyncName) != -1) {
    // タスクを作成する
    autoLipsync = true;
    xTaskCreateUniversal(
      taskLipsyncLoop,  // Function to implement the task
      "taskLipsyncLoop",// Name of the task
      2048,             // Stack size in words
      ctx,              // Task input parameter
      2,                // Priority of the task
      NULL,             // Task handle.
      CONFIG_ARDUINO_RUNNING_CORE);
  }
}

// リップシンクを終了する
void Zundavatar::stopAutoLipsync() {
  if (autoLipsync) {
    // タスクを削除する（実際はタスク内で処理）
    autoLipsync = false;
  }
}

} //namespace
