# reliable-udp-arq

[![CI](https://github.com/erencicekk25/./actions/workflows/ci.yml/badge.svg)](https://github.com/erencicekk25/./actions/workflows/ci.yml)
![Language](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
[![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

UDP üzerinde TCP benzeri güvenilir veri aktarımı yapan küçük bir eğitim projesi.

UDP paketleri kaybedebilir, sırasını karıştırabilir ya da aynı paketi iki kez teslim edebilir. Bu projede UDP'nin üstüne ince bir katman ekleyerek bu sorunları **ARQ (Automatic Repeat reQuest)** yöntemiyle çözüyoruz. Amaç, TCP'nin içinde neler olduğunu basit bir şekilde görmek.

## Ne yapıyor?

- Bağlantı kurma (SYN → SYN|ACK) ve kapatma (FIN → FIN|ACK)
- Her pakete bir sıra numarası (`seq`) verme
- Alıcının "şu numaraya kadar her şeyi aldım" diye ACK göndermesi (cumulative ACK)
- Belirli sürede ACK gelmezse paketleri tekrar gönderme (timeout + retransmission)
- Tekrar gelen (duplicate) ve sırası bozuk paketleri ayıklama
- Checksum ile bozuk paketleri atma
- Paket kaybını taklit etmek için `--loss` seçeneği
- Olan biten her şeyi konsola log olarak basma

## ARQ mantığı kısaca

### Stop-and-Wait

En basit hali. Gönderici bir paket yollar ve ACK gelene kadar bekler. ACK gelirse sıradakini yollar, süre dolarsa aynı paketi tekrar yollar.

```
Gönderici                Alıcı
   | --- DATA seq=1 ---->  |
   | <---- ACK ack=2 ----  |
   | --- DATA seq=2 --X    |   (paket kayboldu)
   |   ... timeout ...     |
   | --- DATA seq=2 ---->  |   (tekrar gönderildi)
   | <---- ACK ack=3 ----  |
```

Basit ama yavaş, çünkü her paketten sonra bir tur boyunca bekliyoruz.

### Sliding Window (Go-Back-N)

Gönderici ACK beklemeden aynı anda `N` paketi yola çıkarabilir (`N` = pencere boyutu). Alıcı yalnızca sıradaki paketi kabul eder. Sırası bozuk gelen paketi atar ve en son doğru aldığı yeri tekrar ACK'ler. Süre dolarsa gönderici, onaylanmamış **tüm penceredeki** paketleri yeniden yollar. Adı da buradan geliyor: "N adım geri dön".

```
Pencere = 4
   | --- seq=1 ---> |
   | --- seq=2 --X  |   (kayıp)
   | --- seq=3 ---> |   alıcı: "2'yi bekliyordum" -> ACK ack=2
   | --- seq=4 ---> |   alıcı: yine ACK ack=2
   |  ... timeout ...
   | --- seq=2,3,4 tekrar ---> |
```

Bu projede `--window 1` verirsen Stop-and-Wait, daha büyük bir sayı verirsen Go-Back-N çalışır. Kod aynı, sadece pencere boyutu değişiyor.

## Paket yapısı

Her paketin başında 14 byte'lık bir başlık var (`include/ProtocolHeader.hpp`). Araya padding girmesin diye `#pragma pack(push, 1)` kullanılıyor.

| Alan       | Boyut  | Açıklama                                          |
|------------|--------|---------------------------------------------------|
| `seq`      | 4 byte | Paketin sıra numarası                             |
| `ack`      | 4 byte | Alıcının beklediği bir sonraki sıra numarası      |
| `flags`    | 1 byte | `SYN`, `ACK`, `DATA`, `FIN` (birleştirilebilir)   |
| `reserved` | 1 byte | Şimdilik 0                                        |
| `length`   | 2 byte | Veri uzunluğu (en fazla 1024 byte)                |
| `checksum` | 2 byte | Başlık + veri üzerinden Internet checksum         |

Tüm sayılar ağa network byte order (big-endian) olarak yazılıyor.

## Dosyalar

```
CMakeLists.txt
include/
  ProtocolHeader.hpp   -> paket başlığı, bayraklar, checksum
  ReliableSocket.hpp   -> güvenilir soket sınıfı
  Logger.hpp           -> renkli, zaman damgalı konsol logları
src/
  ReliableSocket.cpp   -> asıl protokol mantığı
  sender.cpp           -> gönderici programı
  receiver.cpp         -> alıcı programı
tests/
  ArqTests.cpp         -> birim testleri + loopback üzerinde uçtan uca aktarım testleri
.github/workflows/
  ci.yml               -> Linux / macOS + sanitizer CI
```

`ReliableSocket` içindeki önemli kısımlar:

- `connect()` / `accept()`: el sıkışma. SYN|ACK kaybolursa SYN tekrar gönderilir.
- `send()`: kayan pencere, zamanlayıcı ve yeniden gönderme burada. Bekleme işi `poll()` ile yapılıyor, yani süre dolunca `poll` geri dönüyor ve paketler tekrar yollanıyor.
- `receive()`: sıra kontrolü, duplicate filtreleme, ACK gönderme.
- `close()`: FIN el sıkışması. Alıcı, kendi FIN|ACK'i kaybolursa diye biraz daha bekliyor (TCP'deki TIME_WAIT'e benziyor).

## Derleme

POSIX soketleri (`sys/socket.h`) kullanıldığı için **Linux, macOS ya da WSL** gerekiyor. Windows'ta doğrudan derlenmiyor.

Gerekenler: CMake 3.15+ ve C++17 destekleyen bir derleyici (GCC 8+ ya da Clang 7+).

```bash
cmake -S . -B build
cmake --build build
```

Derleme bitince `build/sender` ve `build/receiver` oluşur.

### Testler

```bash
ctest --test-dir build --output-on-failure
```

Testler iki parçadan oluşuyor. Birinci kısım saf protokol mantığını kontrol ediyor: başlığın 14 bayt ve big-endian olması, checksum'ın RFC 1071'deki örnekle birebir aynı sonucu vermesi, tek bir bitin değişmesini yakalaması ve sıra numarası karşılaştırmasının `0xFFFFFFFF → 0` taşmasında doğru çalışması.

İkinci kısım gerçek bir alıcı ve göndericiyi aynı süreçte, loopback üzerinde konuşturuyor. Hem Stop-and-Wait (pencere 1) hem Go-Back-N (pencere 8) için önce temiz bir hatta, sonra paketlerin %20'sinin iki yönde de rastgele düşürüldüğü bir hatta aktarım yapılıyor. Her seferinde alınan verinin gönderilenle bayt bayt aynı olduğu ve kayıplı hatta gerçekten yeniden gönderim yapıldığı kontrol ediliyor. GitHub Actions bu testleri her push'ta Linux ve macOS'ta, ayrıca ASan + UBSan ile çalıştırıyor.

## Çalıştırma

İki terminal aç.

**1. terminal: alıcı**

```bash
./build/receiver 9000 -o alinan.txt
```

**2. terminal: gönderici**

```bash
./build/sender 127.0.0.1 9000 -f README.md
```

Kısa bir mesaj göndermek için:

```bash
./build/sender 127.0.0.1 9000 -m "merhaba dunya"
```

Dosya vermezsen gönderici stdin'den okur:

```bash
cat bir_dosya.bin | ./build/sender 127.0.0.1 9000
```

Alıcıya `-o` vermezsen gelen veri stdout'a yazılır. Loglar stderr'e gittiği için şu da çalışır:

```bash
./build/receiver 9000 > cikti.bin
```

### Paket kaybını denemek

Localhost'ta neredeyse hiç paket kaybolmaz. Bu yüzden `--loss` ile paketleri bilerek düşürebilirsin. Örneğin `0.2` her paketin %20 ihtimalle kaybolması demek.

```bash
# alıcı: ACK'lerin %10'u kaybolsun
./build/receiver 9000 -o alinan.txt --loss 0.1

# gönderici: veri paketlerinin %20'si kaybolsun, pencere 8
./build/sender 127.0.0.1 9000 -f README.md --loss 0.2 --window 8
```

Stop-and-Wait'i görmek için:

```bash
./build/sender 127.0.0.1 9000 -f README.md --loss 0.2 --window 1
```

Bitince iki tarafın da en altta yazdırdığı `stream fingerprint` değerine bak. İki değer aynıysa veri eksiksiz ve doğru sırayla gelmiş demektir. İstersen dosyaları doğrudan da karşılaştırabilirsin:

```bash
cmp README.md alinan.txt && echo "ayni"
```

### Seçenekler

Gönderici (`sender <host> <port> [seçenekler]`):

| Seçenek             | Varsayılan | Açıklama                                     |
|---------------------|------------|----------------------------------------------|
| `-f, --file`        | stdin      | Gönderilecek dosya                           |
| `-m, --message`     | -          | Gönderilecek metin                           |
| `-w, --window`      | 4          | Pencere boyutu (1 = Stop-and-Wait)           |
| `-t, --timeout`     | 300        | Yeniden gönderme süresi (ms)                 |
| `-r, --retries`     | 10         | Üst üste kaç timeout'tan sonra vazgeçilsin   |
| `-l, --loss`        | 0          | Yapay kayıp oranı (0 ile 0.99 arası)         |
| `-q, --quiet`       | -          | Sadece hataları ve özeti göster              |

Alıcı (`receiver <port> [seçenekler]`):

| Seçenek             | Varsayılan | Açıklama                                     |
|---------------------|------------|----------------------------------------------|
| `-o, --output`      | stdout     | Gelen verinin yazılacağı dosya               |
| `-t, --timeout`     | 300        | Kapanışta bekleme için kullanılan süre (ms)  |
| `-i, --idle`        | 30000      | Gönderici bu kadar susarsa bağlantıyı kes    |
| `-l, --loss`        | 0          | Giden ACK'ler için yapay kayıp oranı         |
| `-q, --quiet`       | -          | Sadece hataları ve özeti göster              |

## Log örneği

`--loss 0.2 --window 4` ile gönderici tarafında şuna benzer şeyler görürsün:

```
[    0.001] [INFO ] Connecting to 127.0.0.1:9000 (ISN=2841023311)
[    0.001] [SEND ] SYN seq=2841023311
[    0.002] [RECV ] SYN|ACK seq=99120 ack=2841023312
[    0.002] [INFO ] Connection established with 127.0.0.1:9000
[    0.002] [SEND ] DATA seq=2841023312 len=1024
[    0.002] [LOSS ] Simulated network loss: DATA seq=2841023313 len=1024 was dropped
[    0.002] [SEND ] DATA seq=2841023314 len=1024
[    0.003] [RECV ] ACK ack=2841023313 -> 1 packet(s) confirmed, 2 still in flight
[    0.003] [DUP  ] Duplicate ACK ack=2841023313 (window base=2841023313)
[    0.303] [WARN ] Timeout waiting for ACK of seq=2841023313 -> resending 3 packet(s) (retry 1/10)
[    0.303] [RETX ] DATA seq=2841023313 len=1024
...
```

Etiketler:

- `SEND` / `RECV`: normal gönderme / alma
- `LOSS`: `--loss` yüzünden bilerek düşürülen paket
- `RETX`: yeniden gönderilen paket
- `DUP`: tekrar gelen paket ya da ACK
- `WARN`: timeout, sırası bozuk paket, checksum hatası gibi durumlar
- `ERROR`: programın durmasına sebep olan hata

## Bilinen eksikler

Bu bir öğrenme projesi, gerçek TCP'nin yaptığı birçok şey yok:

- Tek bağlantı destekleniyor (alıcı bir göndericiyi kabul ediyor).
- Veri tek yönlü akıyor (sender → receiver).
- Timeout sabit. TCP gibi RTT ölçüp ayarlamıyor.
- Tıkanıklık kontrolü (congestion control) ve fast retransmit yok.
- Go-Back-N kullanıldığı için sırası bozuk gelen paketler saklanmıyor, atılıyor. Selective Repeat bunu daha verimli yapar, bir sonraki adım olarak eklenebilir.
- Sadece IPv4.
