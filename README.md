# LeftPad

iPad（13インチ想定）とBluetoothで接続し、iOSから**外付けキーボード（BLE HID Keyboard）**として認識される左手用入力デバイスのファームウェアプロジェクトです。  
物理ボタンとロータリーエンコーダを用いて、アプリのショートカット操作（ズーム、ブラシサイズ、ツール切替など）を快適に行うことを目的としています。

---

## 2. 対象デバイス

- BLE HID Keyboard として動作するマイコンボード（例：seed studio XIAO-nRF52840）
- 物理ボタン（10個）
- ロータリーエンコーダ（A/B相、割り込みでtick）
- LiPoバッテリー

---

## 3. リポジトリ構成

```text
.
├─ README.md
├─ firmware/
│  └─ LeftPad/
│     └─ LeftPad.ino
├─ hardware/
│  └─ 3D_Print/
