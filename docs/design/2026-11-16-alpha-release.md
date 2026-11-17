# Alpha Sürümü — Yedekleme, Profiller ve Terminal Arayüzü

Durum: **uygulanıyor — aşama 4 kodu tamam; menü ve UAC devri gerçek terminalde elle denenecek** · 2026-11-17 · Hedef sürüm: `0.1.0-alpha.1`

Onay durumu: terminal arayüzü (yol 2), profil tablosu ve güvenlik duvarı
kapsamı Arda tarafından onaylandı. Güvenlik duvarı Arda'nın kararıyla tüm
profillere genişletildi (aşağıya bakın).

## Amaç ve neden

Arayüz (GUI) dışında programın asıl işini yapan ilk sürüm: mevcut durumu
gösterir, bir profil seçtirir, neyin değişeceğini önceden gösterir, yedek alıp
uygular ve tek adımda geri alır. Arda'nın arkadaşı bu sürümü kendi
bilgisayarında dener.

GUI bilerek sona bırakıldı: terminal üzerinden ilerlemek, asıl riskli kısmı
(sisteme yazma ve geri alma) daha hızlı olgunlaştırır. Bu sürümde yazılan
her şey GUI geldiğinde aynen kullanılır; yalnızca terminal ekranı değişir.

## Hedefler

- Milestone 4 (yedekleme/geri alma) ve Milestone 5 (profiller) tamamlanır.
- Üç profil uygulanabilir: Privacy, Performance, Recommended.
- Hiçbir değişiklik, önizleme ve açık onay olmadan yapılmaz.
- Uygulanan her şey geri alınabilir; program kapanıp açılsa bile.
- Terminalde düzenli, renkli, ok tuşlarıyla gezilen bir menü.

## Kapsam dışı

- GUI (Milestone 7) ve tek tek ayar açıp kapatma (Milestone 6)
- Enterprise özellikleri, şirkete özel profiller, filtre listeleri
- TCP genel ayarlarını **değiştirmek** — alpha'da yalnızca gösterilir (aşağıya bakın)
- Ağ kartı sürücüsüne özel ayarlar (güç tasarrufu, interrupt moderation)
- Legacy hattı

## Asla olmaması gerekenler

- Yedeği alınmamış bir değerin değiştirilmesi.
- Geri alındığında sistemin öncekinden farklı kalması ("ayarlanmamış" bir
  değer geri alınınca yine ayarlanmamış olmalı, sıfır yazılmış değil).
- Bir profil uygulandıktan sonra internetin çalışmaz halde bırakılması.
- ~~Privacy profilinde, kullanıcıya sormadan şifresiz DNS'e düşülmesi.~~
  Arda 2026-11-17'de kaldırdı: şifreli DNS çalışmazsa tüm profillerde
  (Privacy dahil) sormadan eski DNS'e dönülür ve kullanıcıya bilgi verilir
  (`2026-11-17-dns-doh-5a.md`).
- Geliştirme sırasında Arda'nın kendi ayarlarının değişmesi.

## Değerlendirilen yollar

### Terminal arayüzü

| Yol | Nasıl görünür | Artısı / eksisi |
|-----|---------------|-----------------|
| 1. Düz çıktı + numaralı menü | "1) Scan 2) Apply…" yazıp Enter | En az iş; ama salt metin, istenen bu değil |
| **2. Yönlendirmeli menü (öneri)** | Ok tuşlarıyla seçim, renkli tablolar, onay soruları | Derli toplu görünür, az bağımlılık, GUI gelince atılacak kod az |
| 3. Tam ekran panel arayüzü | Pencereli, bölmeli, canlı güncellenen ekran | En şık; ama fiilen ikinci bir arayüz yazmak demek, GUI gelince çöpe gider |

Seçilen: **2** (Arda onayladı).

```
 NetShutUp 0.1.0-alpha.1            Windows 11 (build 26200) · not elevated

 ? What would you like to do?
 > Scan current settings
   Apply a profile
   Revert last change
   Exit

 ? Choose a profile
   Privacy       Encrypted DNS, telemetry and name-leak protections
   Performance   Lower latency for games and real-time apps
 > Recommended   Safe, widely useful tweaks; undoes known-harmful ones

 Recommended — 4 of 9 settings will change

   Setting                   Current            After
   Static DNS servers        9.9.9.9, …         1.1.1.1, 1.0.0.1
   Encrypted DNS (DoH)       off                on
   Delivery Optimization     peer-to-peer       off
   Nagle: no delay           not set            on
   (5 settings already match)

 ? Apply these 4 changes? A backup is saved first.  (y/N)
```

### Profillerin tutulma biçimi

| Yol | Artısı / eksisi |
|-----|-----------------|
| Rust kodunda sabit | En basit; ama şirkete özel profil ileride mümkün olmaz |
| **`.exe` içine gömülü veri dosyaları (seçilen)** | Profil = katalog kimliklerine değer atayan küçük dosya. Tek `.exe` korunur; ileride dışarıdan profil yüklemek aynı biçimi kullanır |
| Yanında duran dış dosyalar | Tek `.exe` ilkesini bozar; alpha için gereksiz |

### Yönetici yetkisi

| Yol | Artısı / eksisi |
|-----|-----------------|
| Program her zaman yönetici olarak açılır | Basit; ama yalnızca bakmak isteyen de UAC görür |
| **Gerektiğinde yükseltme (seçilen)** | Tarama yetkisiz çalışır; "uygula" ya da "geri al" seçilince program kendini UAC ile yeniden başlatır |

## Tasarım

### Yedekleme ve geri alma

- Katalogdaki her ayar okunabildiği gibi yazılabilir de olur (yazılamayanlar
  "salt okunur" olarak işaretlenir).
- Uygulamadan önce, değişecek her ayarın o anki durumu bir yedek dosyasına
  yazılır: `%ProgramData%\NetShutUp\backups\<tarih-saat>.json`.
  Kayıt: ayar kimliği, (varsa) adapter GUID'i, önceki durum — *değer* ya da
  *ayarlanmamış*.
- Yedek dosyası diske yazılıp doğrulanmadan hiçbir değişiklik başlamaz.
- Geri alma, yedekteki her kaydı eski haline döndürür; "ayarlanmamış" olan
  değer silinir.
- Bir ayar yazılamazsa uygulama durur ve o ana kadar yapılanlar otomatik
  geri alınır. Kullanıcıya ne olduğu söylenir.
- Yedekler silinmez; "son değişikliği geri al" en yenisini kullanır.

### Profiller

Her satırın kayıt defteri yolu, varsayılanı ve etkisi kodlanmadan önce
Microsoft belgelerinden doğrulandı; doğrulanamayan satır alpha'ya girmedi.
Profiller `modern/profiles/*.json` dosyalarındadır ve `.exe`'ye gömülür.

| Ayar | Privacy | Performance | Recommended | Yan etki riski |
|------|:-------:|:-----------:|:-----------:|----------------|
| DNS sağlayıcısı (aşama 5) | Quad9 | en hızlı ölçülen | Cloudflare (ölçüm başka birini açıkça öne çıkarırsa o) | Düşük |
| Şifreli DNS (DoH) (aşama 5) | açık, şifresize düşmek yasak | açık | açık | Düşük; aşağıya bakın |
| Güvenlik duvarı (aşama 6) | temel + sıkı (onayla) | temel | temel | Aşağıya bakın |
| Telemetri servisi (DiagTrack) | kapalı | — | — | Düşük |
| Telemetri düzeyi ilkesi | en düşük | — | — | Düşük; Ayarlar'da "kuruluşunuz yönetiyor" yazar, Insider sürümleri alınamaz |
| LLMNR (yerel ağda isim yayını) | kapalı | — | kapalı | Düşük |
| Delivery Optimization (güncellemeleri başkalarına yükleme) | kapalı | kapalı | kapalı | Düşük |
| Gecikmeli ACK (TcpAckFrequency) | — | kapalı | — | Düşük; yeniden başlatma ister, yalnızca TCP kullanan oyun/uygulamada fark eder |
| Ağ kısıtlaması (NetworkThrottlingIndex) | — | kapalı | — | Düşük |

Doğrulama notları:

- **Telemetri düzeyi** `AllowTelemetry = 0` yazılır. Microsoft'a göre 0
  yalnızca Enterprise/Education'da "kapalı" demektir; Home ve Pro'da 1
  ("zorunlu veriler") ile eşdeğerdir. Yani her sürümde izin verilen en düşük
  düzey; Home'da telemetriyi tamamen kapatmaz ve program bunu iddia etmez.
- **LLMNR** `EnableMulticast = 0`, **Delivery Optimization**
  `DODownloadMode = 0` (yalnızca HTTP, eşler arası yok).
- **TcpAckFrequency** Microsoft belgesinde var (varsayılan 2, yeniden
  başlatma gerekir).

Bilerek alınmayanlar:

- **`TCPNoDelay` (adaptör başına)** — YouTube rehberlerinin "Nagle'ı kapat"
  diye yazdırdığı ikinci değer. Microsoft bu değeri yalnızca MSMQ için
  belgeliyor; adaptör anahtarı altında bir etkisi olduğuna dair resmî kaynak
  bulunamadı. Katalogda kalır (kullanıcı görsün diye), profile girmez.
- **Akıllı çoklu-ağ isim çözümünü kapatmak** — ilkenin Home sürümünde
  uygulandığı ve Windows 10/11'de VPN DNS sızıntısını gerçekten önlediği
  doğrulanamadı. Sızıntı koruması DoH ile sağlanır.
- **QoS bant genişliği "tweak"i** — modern Windows'ta etkisiz.
- **Auto-tuning'i kapatmak** — indirme hızını düşürür; zararlı.
- **NetBIOS'u kapatmak, NCSI bağlantı denetimini kapatmak** — eski ağ
  paylaşımlarını ve Windows'un "internet var" algısını bozabilir. Milestone 6'da
  tek tek seçilebilir ayar olarak gelir, profile girmez.
- **SystemResponsiveness** — ağ ayarı değil.

Recommended ayrıca şunu yapar: auto-tuning ya da RSS, bilinen zararlı bir
değere çekilmişse kullanıcıyı uyarır. Alpha'da bunu otomatik düzeltmez;
çünkü TCP genel ayarlarının güvenli yazma yolu henüz doğrulanmadı.

### DNS ve şifreleme

- Windows, Cloudflare / Google / Quad9 için DoH adreslerini zaten tanıyor
  (bu makinede doğrulandı). Program DNS sunucusunu ayarlar ve o sunucu için
  DoH'u etkinleştirir.
- Uyguladıktan sonra program gerçek bir isim çözme denemesi yapar.
  Başarısızsa sırayla: (1) aynı profilin izin verdiği başka bir sağlayıcıyı
  DoH ile dener, (2) hiçbiri çalışmazsa DNS ayarını yedekten geri alır ve
  kullanıcıya bildirir. İnternet çalışmaz halde bırakılmaz.
- **DoT (güncellendi, 2026-11-17):** Windows 11'de `netsh dns add encryption
  ... dothost=` ile DoT var (bu makinede doğrulandı). Arda'nın kararı: DoH
  çalışmazsa DoT denenir, o da olmazsa eski DNS. DoT **aşama 5b**'de gelir;
  5a yalnızca DoH'tur.
- **Windows 10:** kararlı DoH yok; DNS adımı atlanır, şifresiz sağlayıcı
  ayarlanmaz (`2026-11-17-dns-doh-5a.md`).

Not: DoT, DoH'a göre daha sık engellenir (853 portu, DoH 443'ü kullanır), bu
yüzden zincirde DoH'tan sonra gelir. Ayrıntı ve akış: `2026-11-17-dns-doh-5a.md`.

### Güvenlik duvarı (tüm profiller, iki kademe)

Arda'nın kararı: güvenlik duvarı ayarları tüm profillerde olsun; Recommended
ve Performance'ta günlük kullanımı bozmayacak kadar, Privacy'de daha sıkı ve
kullanıcının onayıyla.

**Temel kademe — tüm profiller.** Windows'un kendi varsayılanına döndürür,
yeni bir kısıtlama getirmez:

1. Windows Güvenlik Duvarı üç ağ türünde de (Domain / Private / Public) açık
   ve gelen bağlantılar varsayılan olarak engelli olsun. Mevcut izin
   kuralları (oyunlar, paylaşım, yazıcı) olduğu gibi çalışmaya devam eder.

**Sıkı kademe — yalnızca Privacy, ayrıca sorularak.** Normal önizlemenin
parçası değildir; kullanıcı her birine ayrı ayrı "evet" der:

2. Telemetri servisinin dışarıya bağlanmasını engelleyen, programın kendi
   adını taşıyan bir kural.
3. "Public" ağlarda ağ keşfi, dosya/yazıcı paylaşımı ve uzaktan yardım için
   hazır gelen kuralların kapatılması.

Aşama 6'da doğrulanacak: bilgisayarda başka bir güvenlik duvarı (antivirüs
paketi) kayıtlıysa Windows Güvenlik Duvarı'nı o yönetiyor olabilir. Bu durumda
temel kademe atlanır ve kullanıcıya nedeni söylenir; iki duvarın çakışması
bağlantıyı bozabilir.

Yapılmayacak: **alan adına göre engelleme**. Windows Güvenlik Duvarı alan adı
bilmez, yalnızca IP / program / servis bilir; telemetri sunucularının IP'leri
sürekli değişir. Alan adına göre engelleme DNS filtrelemenin işidir ve
enterprise için düşünülen özel DNS fikrine bağlanır.

Geri alma: programın eklediği kurallar kendi grup adıyla işaretlenir ve geri
almada silinir; kapatılan hazır kuralların önceki durumu yedeğe yazılır.

Risk: temel kademe düşük, sıkı kademe orta. Tüm sürümde yan etki üretme
ihtimali en yüksek kısım madde 3'tür (paylaşım, yazıcı, cihaza yayın
bozulabilir); bu yüzden ayrı bir aşamadır ve ayrıca sorulur.

### Terminal menüsü (aşama 4)

Arda 2026-11-17'de onayladı.

Neden: arkadaşı `.exe`'ye çift tıklayacak; komut ezberlemeden tarama, profil
uygulama ve geri alma yapabilmeli. Aşama 3'teki hâliyle çift tıklama taramayı
basıp pencereyi kapatıyordu.

- **Parametresiz çalıştırma menüyü açar.** Tarama `scan` komutuna taşınır;
  `preview`, `apply`, `apply --dry-run`, `revert` komutları aynen kalır.
  Giriş ya da çıkış bir terminal değilse (yönlendirme, betik) menü açılmaz;
  kullanım metni basılır.
- **Menü:** Scan current settings / Apply a profile / Revert last change /
  Exit. Üstte sürüm, Windows build'i ve yetki durumu yazar. Her işlemden
  sonra ana menüye dönülür.
- **Ayrı "dry-run" maddesi yok.** Profil seçilince önizleme her zaman
  gösterilir; onay sorusunun varsayılanı "Hayır"dır.
- **Yetki.** Apply ya da Revert seçildiğinde program yönetici değilse UAC bir
  kez sorulur; menü yönetici penceresinde seçilen adımdan devam eder ve eski
  pencere kapanır. Önizleme iki kez gösterilmez. İzin verilmezse eski
  pencerede ana menüye dönülür. Komut satırındaki `apply` / `revert` eski
  akışını korur (ikinci pencere işi yapar, ilki bekler).
- **Renk.** Varsayılandan farklı ve değişecek değerler vurgulanır. Çıktı bir
  terminal değilse ya da `NO_COLOR` ayarlıysa renk kodu basılmaz.
- **Tuşlar.** Esc bir adım geri gider (ana menüde çıkar), Ctrl+C çıkar.

Değerlendirilip elenenler: menüde de "ikinci pencere işi yapar" akışını
korumak (daha az iş, ama iki pencere arasında gidip gelinir, önizleme iki kez
görünür); programı baştan yönetici olarak açmak (yukarıda reddedildi).

Kütüphaneler: `inquire` 0.9.5 (ok tuşlu seçim ve onay) ve `crossterm` 0.29
(renk). Menü akışı, tuş okumadan bağımsız saf mantık olarak yazılır ve
testleri öyle yapılır; gerçek tuş okuyan katman ince tutulur.

### Geliştirme sırasında güvenlik

- Yazma ve geri alma kodu, programın kendi oluşturduğu zararsız bir deneme
  anahtarı üzerinde test edilir; gerçek ayarlara dokunmaz.
- Arda'nın makinesinde gerçek bir profil uygulaması, yalnızca Arda'nın o an
  verdiği açık onayla yapılır.
- Program `--dry-run` ile çalıştırılırsa her şeyi gösterir ama hiçbir şey
  yazmaz. Arkadaşın ilk denemesi için önerilir.

### Aşamalar

Her aşama kendi başına çalışır, test edilir ve commit edilir:

1. ✅ Yazma + yedekleme + geri alma motoru (deneme anahtarında testlerle)
2. ✅ Yeni katalog ayarları (doğrulanarak) + gömülü profiller + önizleme
   (`netshutup-modern.exe preview <profil>`; hiçbir şey yazmaz)
3. Yönetici yükseltme + servis yazma desteği + uygulama/geri alma akışı
   — kod tamam (`apply`, `apply --dry-run`, `revert`); gerçek bir makinede
   apply → revert doğrulaması bekliyor
4. Terminal menüsü — kod ve akış testleri tamam (`menu.rs` sahte arayüzle test
   edilir); `inquire` çizimi ve UAC devri henüz gerçek bir terminalde elle
   denenmedi
5. DNS sağlayıcısı + DoH + bağlantı denemesi ve otomatik geri dönüş
6. Güvenlik duvarı (temel kademe tüm profiller, sıkı kademe Privacy)
7. Sürüm numarası, README'de kullanım ve "alpha" uyarısı, GitHub ön-sürüm

## Claude'un verdiği kararlar

Konuşmada kararlaştırılmadı; itiraz edilebilir:

- Sürüm adı `0.1.0-alpha.1`. ("Alpha" = özellikler eksik; "beta" = özellikler
  tam, hata ayıklanıyor. GUI olmadığı için alpha doğru ad.)
- Yedekler `%ProgramData%\NetShutUp\backups` altında, JSON.
- Terminal için ek kütüphaneler (`inquire`, `crossterm`) ve JSON için
  `serde` / `serde_json` eklenir. Hepsi `.exe`'nin içine derlenir.
- Program metinleri İngilizce kalır.
- Profil listesindeki ayar seçimleri ve "bilerek alınmayanlar".
- TCP genel ayarları alpha'da salt okunur.
- Yedek kısmen başarısız olursa otomatik geri alma.

- Profil ayarları yalnızca gerçek donanım olan Ethernet ve Wi-Fi adaptörlerine
  uygulanır (tünel ve diğer sanal türler atlanır). Windows bazı sanal
  adaptörleri (Wi-Fi Direct, WAN Miniport) Wi-Fi/Ethernet türünde bildirdiği
  için tür tek başına yetmez; Windows'un "donanım arayüzü" işaretine bakılır.
- Doğrulanamayan iki aday (`TCPNoDelay`, akıllı çoklu-ağ isim çözümü)
  profillerden çıkarıldı.

## Açık sorular

Yok. Önceki ikisi (terminal arayüzü, profil tablosu) Arda'nın onayıyla kapandı.
