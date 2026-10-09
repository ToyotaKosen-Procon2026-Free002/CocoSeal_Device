# サーバー同期API仕様

## ファームウェアの動作

子機は`https://coco-seal.mydns.jp`に接続し、次のエンドポイントを使用します。

- `POST /devices/activate`：デバイスUUIDとP-256公開鍵を登録します。
- `GET /devices/device`：デバイス署名付きで自分のデバイス情報を取得し、登録表示名を無線で送信します。
- `GET /devices/trading_seals`：子機署名付きでサーバー上の交換プールを取得します。レスポンスの各`seal_id`を端末の交換可能在庫へ反映します。
- `POST /devices/status`：電池残量を送信し、すれ違いまたは交換完了イベントがあれば1件ずつ同時に送信します。

ファームウェアは公開鍵とECDSA署名を小文字の16進文字列で送信します。`SERVER_API_HEX_FIELDS=0`ではサーバー同期が無効になり、互換APIをサーバーにデプロイした環境では`SERVER_API_HEX_FIELDS=1`に設定してください。

ファームウェアはイベントにECDSA P-256 / SHA-256署名を付けます。認証が必要なHTTPリクエストでは`X-Device-Id`と`X-Device-Signature`ヘッダーを使用します。後者は、`device_id`のUTF-8バイト列と**実際に送信するJSONリクエスト本文のバイト列**を連結したデータに対するDER形式の署名を、小文字の16進数にした値です。

Wi-Fi接続または再接続後、ファームウェアは`/devices/status`へ`battery`を含むリクエストを送ります。イベントがない場合も`nearby_communications`を空配列にして送信し、`battery`は0〜100のパーセント値です。イベントを送る場合も同じリクエストに最新の電池残量を含めます。したがって、サーバーの`DeviceUpdateRequest`が要求する`battery`項目に対応しています。

`GET /devices/device`と`GET /devices/trading_seals`にはJSON本文がないため、`X-Device-Signature`はデバイスIDのUTF-8バイト列だけを署名します。`/devices/device`の`Device.name`を自身の無線名として使用します。レスポンスの`DeviceSeal`配列に同じ`seal_id`が複数ある場合、その出現数を交換可能枚数として扱います。同期に成功したときだけローカルの交換プールをサーバー内容に置き換え、サーバーアクセスに失敗した場合は既存のローカル在庫を保持します。

## ESP-NOWデバイス名通知

端末は従来の64バイト`CommunicationPacket`による遭遇・SOS通知を維持しつつ、名前通知では同じ64バイト長の別レイアウトを使用します。`device_id`はオフセット0、32-bit `type=2`はオフセット40、UTF-8名（最大19バイト＋終端NUL）はオフセット44です。遭遇またはSOS通知の直前に名前通知をブロードキャストします。受信側は送信元IDと名前を一時キャッシュし、遭遇表示で名前を使います。親機でも名前を送受信するには、親機ファームウェアにこの`type=2`パケットの送信・受信対応が必要です。

`/devices/status`のイベント署名対象は、次のUTF-8文字列です。

```text
lower(event_id)|lower(my_id)|lower(partner_id)|gateway_flag|lower(send_seal_id)|lower(receive_seal_id)|timestamp_unix
```

`gateway_flag`は`1`または`0`です。シールIDがない場合、署名対象文字列では空文字列、JSONでは`null`を指定します。`timestamp_unix`はUTCのUNIX時刻（秒）です。JSONの`timestamp`は末尾に`Z`を付けたUTCのISO 8601形式です。
`partner_name`には遭遇時に相手から受信した表示名を含めます。名前通知を受け取っていない既存イベントでは空文字列です。この値はイベント署名のcanonical文字列には含めず、実際のJSON本文を使うリクエスト署名には含めます。
親機から配布されたシールを受信した遭遇では、親機のUUIDを`partner_id`、`partner_is_gateway`を`true`、受信したシールIDを`receive_seal_id`として、子機自身が署名付きで`POST /devices/status`へ送ります。サーバーの`DeviceUpdateRequest`では`device_id`は子機UUID、`request_id`はイベントUUID、`battery`は0〜100、`timestamp`は送信時刻、`nearby_communications`はこの遭遇イベントを含む配列です。正常に登録されたイベントの`receive_seal_id`は、サーバー側の子機在庫へ追加されます。

ESP-NOWの親機配布パケットは64バイトで、`device_id`（37バイト）、`type`（32-bit、オフセット40）、`stickerId`（16バイト、オフセット44）、`isGateway`（オフセット60）です。受信側は`type=0`かつ`isGateway=true`のパケットを親機遭遇として扱い、パケット内の`stickerId`を上記の`receive_seal_id`として記録します。親機は続けて`type=2`、`isGateway=true`の同じレイアウトのパケットを送り、オフセット44の`stickerId`領域にスポット名（最大15 UTF-8バイト）を載せます。子機は親機IDに対応付けて名前をキャッシュし、遭遇表示の「をとおったよ」の前に表示します。

有効化リクエストの例（`public_key`の値は16進数130文字です）。既存の登録名を上書きしないよう、ファームウェアは任意項目の`name`を送らず、`GET /devices/device`で取得した名前を無線通知に使用します。

```json
{
  "device_id": "child-uuid",
  "public_key": "04..."
}
```

状態更新リクエストの例（`signature`はDER形式のECDSA署名を16進数にした値です）。

```json
{
  "device_id": "child-uuid",
  "request_id": "event-uuid",
  "battery": 75.0,
  "timestamp": "2026-09-08T12:00:00Z",
  "nearby_communications": [{
    "event_id": "event-uuid",
    "my_id": "child-uuid",
    "partner_id": "peer-uuid",
    "partner_name": "ココシール子機",
    "partner_is_gateway": false,
    "send_seal_id": null,
    "receive_seal_id": null,
    "timestamp": "2026-09-08T11:59:55Z",
    "signature": "3045..."
  }]
}
```

すれ違いイベントのシールIDは通常どちらも`null`です。ただし、シールIDを通知する親機とのすれ違いでは、そのIDを`receive_seal_id`として送信します。子機間の交換完了イベントでは、交換に出したシールIDを`send_seal_id`、受け取ったシールIDを`receive_seal_id`として送信します。イベントUUIDは再送時も変わらず、リクエストIDとしても使用します。サーバー側では`event_id`を冪等性キーとして扱ってください。

## サーバー側で必要な変更

現行のスキーマでは、`public_key`と`NearbyCommunication.signature`をJSON上の`bytes`として定義しています。Pydanticの標準JSON処理では、文字列を元のバイト列に戻さず、UTF-8文字列として扱います。このため、65バイトの楕円曲線公開鍵やDER署名を、ファームウェアからJSONで安全に送信できません。

次の互換性変更をお願いします。

1. `DeviceInitRequest.public_key`を16進文字列として受け取れるようにしてください。`/devices/activate`では`bytes.fromhex()`で復号し、非圧縮P-256点形式（先頭バイトが`0x04`）の65バイトであることを確認したうえで、SECP256R1の有効な点として検証し、復号したバイト列を保存してください。登録済みデバイスの有効化は、保存済み公開鍵が一致する場合に限り冪等に成功させてください。鍵が一致しない場合は、異なる鍵を黙って受け入れたり置き換えたりせず、エラーにしてください。
2. `NearbyCommunication.signature`を16進文字列として受け取れるようにしてください。既存のECDSA検証処理に渡す前に`bytes.fromhex()`で復号してください。イベント署名対象の生成方法は上記仕様を維持し、登録済みデバイスの公開鍵で署名を検証してください。
3. `/devices/status`のリクエスト認証方式は変更しないでください。`X-Device-Signature`ヘッダーは既に16進形式で定義されており、デバイスIDを先頭に付加した実際のリクエスト本文を検証します。
4. `event_id`を使った遭遇登録の冪等性を維持し、`ON CONFLICT DO NOTHING`で重複登録を防いでください。交換による在庫変更は、遭遇レコードが初めて登録された場合に限って実行してください。親機からシールを受け取るすれ違いについては、子機の現行仕様に合わせ、日本時間の暦日ごとに親機1台あたり最大1枚だけ付与してください。すれ違いイベントごとに付与しないでください。
5. トランザクションのコミットが完了した後に限り、`{"success": true}`を返してください。

## 追加の連携要件

### 親機の初回登録

子機だけでなく、サーバーへSOSを中継する親機も公開鍵を登録し、以降のリクエストに署名する必要があります。現行の`POST /devices/gateway`は`GatewayInitRequest.public_key`をJSON上の`bytes`として受け取るため、子機と同じ問題があります。親機も16進文字列で公開鍵を送れるようにし、復号・P-256公開鍵の検証を行ってから保存してください。登録済みIDについては、既存公開鍵と一致する場合のみ成功させ、異なる鍵への置き換えを拒否してください。

`X-Gateway-Id`と`X-Gateway-Signature`による親機認証は既にあります。ヘッダー署名は現在の仕様（親機IDのUTF-8バイト列と実際のJSON本文を連結したデータへのDER形式ECDSA署名、その16進表現）を維持してください。

### イベント登録の冪等性とDB処理

`update_device_status()`は遭遇行の`INSERT`後に`fetchone()`で登録有無を判定していますが、現行SQLには`RETURNING`句がありません。PostgreSQLで新規挿入と重複を確実に区別できるよう、`RETURNING`で挿入された行を取得するか、同等に明確な方法で判定してください。在庫更新は新規挿入時だけ行い、重複イベントの再送では二重に増減させないでください。遭遇記録と在庫更新は同じDBトランザクションに含め、検証または更新に失敗した場合はロールバックしてください。

ファームウェアは状態更新を1イベントずつ送ります。サーバーが複数イベントを含むリクエストも受け付ける場合は、各イベント単位の検証・重複排除に加え、部分成功を返さないトランザクション方針を定めてください。

### バッテリー情報

`DeviceUpdateRequest`の`battery`は必須項目です。ファームウェアはWi-Fi接続または再接続後、およびすれ違い・交換イベントの送信時に、0〜100のパーセント値とUTCの更新時刻を送ります。イベントを伴わない更新では`nearby_communications`は空配列です。サーバーは既存の`update_device_status()`で`battery`と`last_timestamp`を更新するため、追加のAPI変更は不要です。

## SOS同期

`/devices/sos_gateway`は親機と子機の両方を認証します。親機は必須の`X-Gateway-Id`と`X-Gateway-Signature`ヘッダーを使い、現在のサーバー認証実装では親機IDとリクエスト本文を連結したバイト列に署名します。本文の`signature`は、SOSイベントを発生させた子機の署名です。サーバー側の子機署名対象は`event_id|child_id|trigger_timestamp_unix`（イベントIDと子機IDは小文字）です。

子機と親機は、次の固定長ESP-NOWパケットを共有します。最初の64バイトは従来の`CommunicationPacket`、続くフィールドはイベントID（37バイト）、予約領域（3バイト）、発生Unix時刻（uint32）、DER署名（最大80バイト）、署名長（1バイト）です。合計は192バイトです。子機は信頼できるネットワーク時刻がある場合に限ってイベントIDを生成し、子機秘密鍵で署名して送信します。時刻が信頼できない、または署名に失敗した場合は、親機での緊急アラートを維持するため旧形式のSOSパケットを送りますが、サーバー中継はできません。

署名付きSOSを受信した親機は、イベントID・子機ID・発生時刻・子機署名を本文に含め、親機ID・受信時刻も加えて`/devices/sos_gateway`へ送ります。親機署名は実際に送るJSON本文を対象とします。サーバーがSOSを受理すると既存の通知処理からFCMが送られ、FCM通知を受けたアプリはSOS画面を表示します（子機の所有者として通知トークンが登録されている必要があります）。親機は同じSOSをBLEログにも加え、接続中はLog characteristicへ通知し、未接続なら`GET_LOGS`要求まで保持します。

現在のファームウェアはSOSごとにイベントIDを生成して1回送信し、親機もその場でサーバーへ1回中継します。永続的な再送・親機応答による確認は未実装です。ネットワーク時刻が未同期の場合、Wi-Fiが未接続の場合、またはサーバーがエラーを返した場合は、アプリ向けBLEログは保持されますが、サーバー/FCMへの再送は行われません。確実な再送を追加する場合は、イベントIDと発生時刻の永続化、親機受信の重複排除、サーバーの受信記録の冪等性を合わせて実装してください。
