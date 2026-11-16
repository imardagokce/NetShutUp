# Milestone 3 — Ayar Kataloğu ve Mevcut Durumu Okuma

Durum: **onaylandı, ilk dilim uygulandı** · 2026-11-16

## Amaç ve neden

Program, bir şeyi değiştirmeden önce bilgisayardaki mevcut network ayarlarını
okuyup kullanıcıya gösterir ("Read-Only Diagnostics First"). Bu milestone o
okuma katmanını kurar.

Aynı zamanda sonraki her şeyin temelini atar: yedekleme (M4), profiller (M5),
tek tek ayar yönetimi (M6) ve ileride şirketlere özel profiller, hepsi aynı
ayar listesinin üzerinde çalışacak.

## Hedefler

- Programın bildiği tüm ayarlar tek bir **katalogda** tanımlı olur.
- Her ayarın mevcut değeri okunur ve Windows varsayılanıyla karşılaştırılır.
- Çıktı, ayarın adını, değerini ve varsayılandan farklı olup olmadığını gösterir.
- Yönetici yetkisi gerekmez.

## Kapsam dışı (bu milestone'da yok)

- Herhangi bir ayarı değiştirmek, yedek almak, profil uygulamak
- UAC / yönetici yükseltme
- TCP genel ayarları (autotuning, ECN, RSS) — ikinci dilimde, aşağıya bakın
- Arayüz, JSON çıktısı, şirket/enterprise özellikleri

## Asla olmaması gerekenler

- Sisteme **tek bir yazma işlemi** bile yapılması. Kod yalnızca okuma
  yetkisiyle anahtar/servis açar.
- Windows diline bağlı metin çıktısı ayrıştırmak (Türkçe Windows'ta bozulur).
- Emin olmadığımız bir değere "Windows varsayılanı" demek. Varsayılanı
  doğrulanmamış ayarda etiket gösterilmez.

## Değerlendirilen yollar

| Yol | Artısı | Eksisi |
|-----|--------|--------|
| 1. Her ayar için ayrı kod | En hızlı başlangıç | Her yeni ayar yeni kod; şirkete özel profil mümkün olmaz |
| **2. Kodun içinde katalog (seçilen)** | Tek `.exe` korunur; profiller yalnızca katalogdaki kimliklere değer atar, katalog dışına çıkamaz | Yeni ayar eklemek yeni sürüm gerektirir |
| 3. Katalog dış dosyada | En esnek | Tek `.exe` ilkesini bozar; dosyayı değiştiren biri yönetici yetkili programa istediği anahtarı yazdırabilir |

Seçim: **2**. Karar Arda tarafından Claude'a bırakıldı.

## Tasarım

### Katalog

Her ayar şu bilgilerle tanımlanır:

- **Kimlik** — kalıcı, değişmez (ör. `tcp.nagle.no_delay`). Profiller ve
  yedekler ayarı bu kimlikle tanır.
- **Ad** — kullanıcıya gösterilen isim
- **Kapsam** — sistem geneli ya da adapter başına
- **Kaynak** — nereden okunduğu: kayıt defteri değeri ya da Windows servisi
- **Windows varsayılanı** — biliniyorsa ve doğrulanmışsa

Okuma sonucu üç durumdan biridir: *değer var*, *ayarlanmamış (Windows
varsayılanı geçerli)*, *okunamadı (neden)*.

### İlk dilimdeki ayarlar

| Kimlik | Ne | Kapsam | Kaynak |
|--------|----|--------|--------|
| `tcp.nagle.ack_frequency` | Nagle — ACK sıklığı | Adapter | Kayıt defteri `TcpAckFrequency` |
| `tcp.nagle.no_delay` | Nagle — gecikmesiz gönderim | Adapter | Kayıt defteri `TCPNoDelay` |
| `dns.static_servers` | Elle girilmiş DNS sunucuları | Adapter | Kayıt defteri `NameServer` |
| `mm.network_throttling` | Multimedya ağ kısıtlaması | Sistem | Kayıt defteri `NetworkThrottlingIndex` |
| `mm.system_responsiveness` | Arka plan için ayrılan CPU payı | Sistem | Kayıt defteri `SystemResponsiveness` |
| `qos.reservable_bandwidth` | QoS için ayrılabilir bant genişliği | Sistem | Kayıt defteri (ilke) `NonBestEffortLimit` |
| `service.diagtrack` | Telemetri servisi | Sistem | Servis durumu + başlangıç tipi |

Adapter başına ayarlar, Milestone 2'de alınan adapter GUID'i ile bulunur.
Loopback adapter'ı gösterilmez.

Bir ayarın katalogda olması, onun bir profile gireceği anlamına gelmez.
Örneğin QoS ayarı modern Windows'ta etkisiz bir tweak olarak bilinir;
Recommended'a girmez ama kullanıcı değiştirmişse görebilmelidir.

### Çıktı

```
Settings — Ethernet
  Nagle: ACK frequency       not set        (Windows default)
  Nagle: no delay            not set        (Windows default)
  Static DNS servers         9.9.9.9, 149.112.112.112   (changed from default)

Settings — System
  Network throttling         disabled       (changed from default)
  System responsiveness      20             (Windows default)
  QoS reservable bandwidth   not set        (Windows default)
  Telemetry service          Stopped, Disabled   (changed from default)
```

Bu örnek, bu bilgisayarın şu anki gerçek değerleridir (2026-11-16'da
okundu, hiçbir şey değiştirilmedi).

### Kod yapısı

`main.rs` şu an tek dosya; bölünecek:

```
modern/src/
├── main.rs        ← sadece akış ve yazdırma
├── version.rs     ← Windows sürümü (mevcut kod taşınır)
├── adapters.rs    ← adapter listesi (mevcut kod taşınır)
└── settings/
    ├── catalog.rs ← ayar tanımları
    └── reader.rs  ← kayıt defteri ve servis okuma
```

### Testler

- Katalogdaki kimlikler benzersiz mi
- Okunan değerin "varsayılan / değiştirilmiş" olarak doğru sınıflandırılması
  ve doğru biçimlendirilmesi (sistemden bağımsız, saf mantık)
- Var olmayan bir kayıt defteri değeri "ayarlanmamış" olarak dönüyor mu

Hiçbir test sisteme yazmaz.

## Claude'un verdiği kararlar

Bunlar konuşmada kararlaştırılmadı; itiraz edilebilir:

- Kayıt defteri ve servisler, mevcut `windows` crate'i üzerinden okunur
  (yeni bağımlılık yok). Yeni feature'lar: `Win32_System_Registry`,
  `Win32_System_Services`.
- Program çıktısı İngilizce kalır (mevcut çıktıyla tutarlı).
- İlk dilime DNS ayarı eklendi (konuşulan listede yoktu); Privacy profili
  için temel bir ayar olduğu için.
- Varsayılan değerler kodlanmadan önce Microsoft belgelerinden doğrulanır.
- Tek `.exe` ek DLL istemesin diye C çalışma zamanı statik bağlanır (`modern/.cargo/config.toml`).

## İkinci dilim (ayrı iş)

TCP genel ayarları (autotuning, ECN, RSS). Bunlar kayıt defterinde düz bir
değer olarak durmuyor; Windows'un yönetim arayüzünden (WMI) okunmaları
gerekiyor. Daha karmaşık olduğu için ilk dilim bittikten sonra ele alınacak.
