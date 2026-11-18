# Legacy — Şifreli DNS (yerel DoH çözücü)

Durum: **Arda onayladı (2026-11-18); 7a kodlanıyor**
Karşılığı: `main` dalında `2026-11-17-dns-doh-5a.md` (Windows 10/11, Windows'un kendi DoH'u).
Bu belge yalnızca Windows 7 SP1 / 8 / 8.1 içindir.

## Özet (30 saniyede)

- Win7/8/8.1'in DNS istemcisi DoH/DoT konuşamaz. Şifreli DNS'in tek yolu, bilgisayarda
  **sürekli çalışan yerel bir çözücü**: `127.0.0.1:53` dinler, sorguları DoH ile
  sağlayıcıya (Quad9 / Cloudflare / Google) iletir. Ağ kartlarının DNS'i `127.0.0.1` olur.
- Çözücüyü **biz yazıyoruz** (WinHTTP, yaklaşık 500 satır C++, kendi kodumuz, GPL v3 ile
  sorunsuz). Hazır seçenekler elendi: cloudflared bu özelliği kaldırdı; dnscrypt-proxy
  Win7 için Go 1.20'ye kilitli ve lisansı doğrulanmadı; Acrylic açık kaynak değil.
- Çözücü **aynı `.exe`'nin** bir modu (`netshutup-legacy --dns-service`); ayrı dosya yok.
  `apply` servisi kurar, `revert` servisi ve kopyalanmış `.exe`'yi siler, DNS'i eski haline getirir.
- Önce deneme, sonra geçiş: çalıştığı kanıtlanmadan ağ kartlarına dokunulmaz.
  Deneme başarısızsa **hiçbir şey kurulmaz**, eski DNS kalır, kullanıcı bilgilendirilir.

## Neden bu yol (araştırma özeti)

- Win7'de yerel DoH desteği yok (Windows 10 2004'ten itibaren var). Win8/8.1'de de yok.
- DoT da aynı sorunu taşır (Windows DNS istemcisi konuşmaz), yani her iki yol da yerel
  çözücü ister. Arda'nın kararı: **DoH ve DoT, sırayla** denenir (aşağıda "Zincir").
- Win7 ön koşulları: TLS 1.2 için KB3140245 (WinHTTP `DefaultSecureProtocols`), eski kök
  sertifikalar için KB3004394 / güncel kök deposu. Hiçbiri Win7 makinede **doğrulanmadı**;
  ilk gerçek koşu sanal makinede.

## Hedefler

- Üç profilde şifreli DNS; profiller arası fark yalnızca sağlayıcı sırası
  (Privacy → Quad9, sonra Cloudflare; Recommended → Cloudflare, sonra Google;
  Performance → ölçümle hızlı olan Cloudflare / Google). Modern hat ile aynı.
- Her şey tek yedekle geri alınır (`revert` DNS'i, servisi ve dosyaları geri alır).
- İnternet çalışmaz halde bırakılmaz.
- `--dry-run` hiçbir şey yazmaz; `preview` DNS'in ne olacağını gösterir.
- Mantığın çoğu sahte bileşenlerle test edilir; test sırasında gerçek ağa çıkılmaz.

## Kapsam dışı (v1)

- DNSSEC doğrulama, DNS filtreleme, kullanıcı tanımlı sağlayıcı, enterprise sağlayıcılar.
- IPv6 DNS: v1 yalnızca IPv4 DNS'i `127.0.0.1` yapar (modern hat ile aynı sınır). Yönlendiriciden
  gelen IPv6 DNS şifresiz kalabilir; Privacy'de not düşülür.
- Önbellek: v1'de yok (ilk sürümü basit tutmak için). Gecikme ölçülürse eklenir.
- Windows'un genel TLS/Schannel ayarlarını (kayıt defteri) değiştirmek. Yalnızca kendi
  WinHTTP oturumumuzda TLS 1.2 isteriz.

## Asla olmaması gerekenler

- Yedek yazılıp doğrulanmadan servis kurmak veya DNS'e dokunmak.
- İnternetin çalışmaz halde kalması: servis çökerse yeniden başlar; yeniden başlamazsa
  (aşağıdaki "Çalışma sırasında hata") eski DNS'e düşülür.
- `revert` sonrası DNS'in öncekinden farklı kalması: DHCP'den gelen DNS ("ayarlanmamış")
  yine DHCP'den gelmeli, sabit adres yazılmamalı.
- Şifreli çalışıyormuş gibi gösterilip şifresiz sorgu gitmesi: durum her zaman açıkça
  görünür (aşağıda `dns-status`).
- İnternet baştan yokken DNS'e dokunmak.

## Tasarım

### Parçalar

| Parça | Dosya (öneri) | Sorumluluk |
|-------|---------------|------------|
| Saf çekirdek | `dns_core.h/.cpp` | Yapılandırma, sağlayıcı tablosu, üst sunucu seçimi / yedeğe geçiş durum makinesi, paket yardımcıları (UDP→TCP yeniden deneme için kesilme biti, kimlik koruma). Saf, test edilir. |
| DoH istemcisi | `dns_doh.h/.cpp` | WinHTTP, RFC 8484 `POST application/dns-message`, TLS 1.2, zaman aşımı. Arayüz arkasında; testte sahte. |
| Sunucu | `dns_server.h/.cpp` | `127.0.0.1` üzerinde UDP ve TCP 53 dinleyici, işçi iş parçacıkları (Win32 olayları; `atomic_wait` vb. yasak). |
| Servis | `dns_service.h/.cpp` | `StartServiceCtrlDispatcher`, durdurma, yeniden başlatma eylemleri; `--dns-service` modu. |
| Kurulum | `dns_setup.h/.cpp` | `.exe`'yi `%ProgramData%\NetShutUp\bin\` altına kopyala, servisi kur/sil, bağlantı noktası denetimi. |
| Kart DNS'i | `dns_adapters.h/.cpp` | Kartların mevcut DNS'ini oku (yedek için), `127.0.0.1` yaz, geri yükle. |
| Akış | `dns_apply.h/.cpp` | Plan → yedek → deneme → kurulum → geçiş → doğrulama → hata olursa geri al. |

### Yapılandırma ve sağlayıcılar

- `%ProgramData%\NetShutUp\dns.cfg` (düz metin): sıralı üst sunucu listesi + "yedeğe geçiş"
  ilkesi + eski DNS sunucuları. Servis başlarken okur; `apply` yazar.
- Sağlayıcılar **IP adresiyle** adreslenir (`https://1.1.1.1/dns-query`,
  `https://8.8.8.8/dns-query`, `https://9.9.9.9/dns-query`): isim çözmeye ihtiyaç yok
  (önyükleme sorunu yok); üç sağlayıcının sertifikaları IP SAN taşır. **Win7'de doğrulanmalı.**

### Kart DNS'i ve yedek

- Okuma: `HKLM\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters\Interfaces\{GUID}\NameServer`
  (boş/yok = DHCP'den gelir). Yalnızca `adapters` modülünün döndürdüğü fiziksel kartlar.
- Yazma: `netsh interface ipv4 set dnsservers` **arayüz indeksiyle** (isim yerelleşir; çıktı
  ayrıştırılmaz, yalnızca çıkış kodu). Geri alma: eski değer DHCP ise `source=dhcp`.
- Mevcut `.nsub` yalnızca DWORD taşır; DNS için `<yedek>.dns` yan dosyası (modern hattaki
  `.dot.json` gibi): kart GUID'i, eski `NameServer` (var mı, değeri), servis daha önce var mıydı,
  `.exe` kopyası var mıydı. Yedek yazılıp **geri okunarak doğrulanmadan** kurulum başlamaz.

### Apply akışı (hata olursa tamamı geri alınır)

1. Ön koşullar: Windows sürümü, internet var mı, TLS 1.2 isteği kabul ediliyor mu
   (`WinHttpSetOption` — KB3140245 yoksa reddeder), `127.0.0.1:53` boş mu. Biri olmazsa
   DNS atlanır, **neden söylenir**, profilin kalanı normal uygulanır.
2. Yedek (yan dosya dahil) yazılır ve doğrulanır.
3. Deneme: seçilen sağlayıcıya WinHTTP ile gerçek bir sorgu (ör. `example.com`). Olmazsa
   sıradaki sağlayıcı; hiçbiri olmazsa **hiçbir şey kurulmaz** ve eski DNS kalır.
4. `.exe` kopyalanır, servis kurulur ve başlatılır; `127.0.0.1`'e UDP ve TCP sorgusu atılır.
5. Kartların DNS'i `127.0.0.1` yapılır; internet ve çözme yeniden doğrulanır.
6. Bir adım başarısızsa: kartları eski haline getir, servisi durdur/sil, dosyayı sil.

### Zincir ve çalışma sırasında hata (Arda'nın kararı, 2026-11-18)

Modern hat ile aynı mantık: (sağlayıcı, yöntem) çiftleri sırayla denenir:
**A-DoH → A-DoT → B-DoH → B-DoT → eski DNS**. Sağlayıcı sırası profile göre (Privacy: Quad9,
Cloudflare; Recommended: Cloudflare, Google; Performance: ölçümle hızlı olan önce). Hiçbiri
çalışmazsa **sormadan eski DNS'e dönülür** ve kullanıcı bilgilendirilir.

- **Apply anında:** zincir sırayla denenir; ilk çalışan yöntemle kurulum yapılır. Hiçbiri
  çalışmazsa hiçbir şey kurulmaz, eski DNS kalır.
- **Çalışma sırasında:** servis her sorguyu zincir sırasıyla dener (son çalışan yöntemden
  başlayarak). Zincirin tamamı art arda N tur başarısız olursa (N=3, ayarlanabilir) servis
  kayıtlı **eski DNS'e** geçer (internet kesilmez), durumu `dns.status`'e yazar ve her 5.
  sorguda şifreli zinciri yeniden dener; çalışınca şifreliye döner. Eski DNS kaydı yoksa
  şifreli kalır ve SERVFAIL döner.
- DoT: TCP 853 üzerinde Schannel (SSPI) ile; DoH'tan ayrı aşamada yazılır (7b2). Zincir veri
  modeli baştan her iki yöntemi taşır.
- `netshutup-legacy dns-status`: servis çalışıyor mu, şu an hangi sağlayıcı/yöntem, şifreli mi
  yoksa eski DNS'e mi düşmüş, son hata.
- Servis `LocalService` altında, yalnızca gereken yetkiyle çalışır; çökerse otomatik yeniden başlar.

### Revert

Servisi durdur/sil → kart DNS'ini yedekten geri yaz (DHCP ise DHCP) → `.exe` kopyasını sil →
`dns.cfg`/`dns.status` sil. Yarım kalmış bir kurulum bulunursa (ör. elektrik gitti) `revert`
bunu da temizler.

### Selftest

Faz C'ye DNS adımı: servis kurulur, `127.0.0.1` sorgulanır, geri alınır; başlangıç ile bitiş
karşılaştırması kartların `NameServer` değerlerini ve servisin yokluğunu da kapsar.
Güvenlik ağına kart DNS dökümü eklenir.

## Test stratejisi

- Saf çekirdek: yapılandırma ayrıştırma/yazma (gidiş-dönüş), sağlayıcı sırası, yedeğe geçiş
  durum makinesi (N hata → eski DNS, başarı → şifreliye dön), paket yardımcıları.
- DoH istemcisi: arayüz arkasında sahte (zaman aşımı, HTTP hatası, bozuk yanıt).
- Sunucu: döngü adresinde, sahte üst sunucuyla UDP/TCP gidiş-dönüş; gerçek internet yok.
- Yedek yan dosyası: yaz → oku → karşılaştır; bozuk dosya reddedilir.
- Servis kurma/kart DNS'i yazma gerçek sistemi değiştirdiği için yalnızca derlenir;
  gerçek koşu sanal makinede `selftest` ile.

## Aşamalar (öneri)

| Aşama | İçerik | Gerçek sistem? |
|-------|--------|----------------|
| 7a | `dns_config` (sağlayıcılar, zincir, `dns.cfg`), `dns_failover` (durum makinesi), `dns_packet` (paket yardımcıları) + testler | hayır |
| 7b | `dns_doh` (WinHTTP) + sahte testleri | yalnızca derleme |
| 7b2 | `dns_dot` (Schannel/SSPI, TCP 853) + sahte testleri | yalnızca derleme |
| 7c | `dns_server` + `dns_service` (`--dns-service`) | döngü adresi testi |
| 7d | `dns_setup`, `dns_adapters`, yan dosya yedek, `dns_apply`, önizleme/menü/CLI | **VM gerekir** |
| 7e | selftest entegrasyonu, belgeler, `dns-status` | VM |

## Canlı doğrulama (2026-11-18, Windows 11 geliştirme makinesi)

`tests/manual_dns_probe.exe` (ctest'te değil, elle) her sağlayıcıya DoH ve DoT ile birer
`example.com` sorgusu atar:

- Cloudflare DoH ✅, DoT ✅ · Google DoH ✅, DoT ✅ · Quad9 DoT ✅ (bağlantı yeniden kullanımı dahil).
- **Quad9 DoH ❌:** HTTP 505 (HTTP sürümü desteklenmiyor). WinHTTP'nin HTTP/1.1'i reddediliyor;
  büyük olasılıkla Quad9 yalnızca HTTP/2 konuşuyor (HTTP/2'li bir istemciyle doğrulanmadı).
  WinHTTP'de HTTP/2 yalnızca Windows 10 1607+; Win7/8'de yok. Sonuç: legacy'de **Quad9 DoH
  çalışmaz, zincir Quad9 DoT'a düşer** (Privacy zinciri bu yüzden A-DoH'ta hızlıca başarısız olup
  A-DoT'a geçer; çalışan yöntem hatırlandığı için sonraki sorgular doğrudan DoT'tan başlar).
- Win7'de bu sonuçlar tekrarlanmadı; TLS 1.2 / kök sertifika / IP-SAN riskleri duruyor.

## 7c: yerel sunucu ve servis (kodlandı)

- `dns_server`: UDP + TCP dinleyici (8 UDP işçisi, TCP için kısa ömürlü iş parçacığı, en çok 32 bağlantı),
  her sorgu `forward()` zincirinden geçer. UDP yanıtı 512 bayttan büyükse `TC` ile kesilir, istemci TCP'den tam yanıtı alır.
  Port doluysa "port N is already in use" der ve hiçbir şey açık bırakmaz.
- `dns_plain`: yalnızca eski DNS'e düşme için düz UDP (+ TC olursa TCP).
- `dns_status` / `dns_service`: `dns.status` dosyası (5 sn'de bir, geçici dosya + yer değiştirme), SCM girişi
  (`netshutup-legacy --dns-service`, `127.0.0.1:53`), geliştirici komutu `dns-serve [--bind ip] [--port n] [--config yol]`.
- `Failover` iş parçacığı güvenli yapıldı (dahili mutex).
- **Canlı deneme (Win11, Privacy zinciri, bu makine):** `Resolve-DnsName` ve `nslookup` ile `127.0.0.2:53` üzerinden
  yanıtlar doğru döndü; ham UDP/TCP istemcileriyle A/AAAA/PTR/NXDOMAIN/EDNS de doğru. Quad9 DoH hızla başarısız olup
  Quad9 DoT ile çözüldü (ilk sorgu ~0,1 sn). Not: Windows `nslookup -port=...` bu sunucuyla çalışmadı
  (istemci tarafı sorunu gibi görünüyor; standart 53. port çalışıyor, nedeni araştırılmadı).
- Servis hesabı için `%ProgramData%\NetShutUp` yazma izni (durum dosyası) **7d'de** verilecek.

## 7d: kurulum, geçiş, geri alma (kodlandı; gerçek makinede ÇALIŞTIRILMADI)

- `dns_apply` (saf akış, 14 kural + `revert_dns`) soyut `System` arayüzüne karşı yazıldı; tüm geri alma
  yolları sahte sistemle test edilir (30'a yakın test). Gerçek makineye dokunan hiçbir test yok.
- `dns_backup`: `<yedek>.nsub.dns` yan dosyası (kart GUID'i, arayüz indeksi, statik DNS listesi veya DHCP) ve
  `netsh` argüman üreticileri (kart adı yerelleşir; arayüz **indeksi** kullanılır, yalnızca çıkış kodu okunur).
- `dns_system` (gerçek `System`): SCM ile servis kurma/silme (LocalService, otomatik başlat, çökünce yeniden başlat),
  `.exe` kopyası `%ProgramData%\NetShutUp\bin\`, `dns.cfg`, `netsh` ile kart DNS'i, 53. port denetimi,
  uçtan uca sorgu denemesi. Servis hesabı yalnızca `dns.status` dosyasına yazabilir (dosya düzeyinde izin).
- `dns_flow`: gerçek `apply`/`revert` (CLI ve menü) sonrası DNS aşaması. `apply::apply_profile()` içinden
  **çağrılmaz**: testler ve selftest hiçbir zaman servis kurmaz / DNS değiştirmez (selftest entegrasyonu 7e).
- Başarılı geri almadan sonra yan dosya "boş yedek"le etkisizleştirilir; geri alma eksik kalırsa yan dosya korunur.
- Önceki bir apply'dan kalan servis varsa ve geri alınan yedeğin DNS yan dosyası yoksa, `revert` en yeni yan dosyayı kullanır.
- `dns-status` komutu (salt okunur), önizlemede DNS satırı eklendi.

## Riskler

- Win7'de TLS 1.2 ve kök sertifikalar doğrulanmadı; IP SAN'lı sertifika doğrulaması
  WinHTTP'de beklenen gibi çalışmayabilir (o zaman sağlayıcı ana makine adına geçilir ve
  önyükleme sorunu çözülmelidir).
- 53. bağlantı noktasını ICS, başka bir DNS servisi veya VPN tutuyor olabilir: kurulumdan önce
  denetlenir, doluysa DNS atlanır.
- Win7 desteği bitmiş bir sistem; yamasız makinelerde DoH tek başına yeterli koruma değildir.
- Sürekli çalışan bir servis kurmak, "tek `.exe`, kurulumsuz" ilkesini DNS özelinde
  genişletir: kaldırılabilirlik (`revert`) bu yüzden zorunlu ve self-test'le sınanır.
- Kart DNS'ini `netsh` ile yazmanın Win7'de anında etkili olup olmadığı doğrulanmadı.

## Kararlar

Arda (2026-11-18):
- DoH engellenirse sırayla diğer şifreli yöntemler (DoT), hiçbiri olmazsa eski DNS.
- `.exe` kopyası `%ProgramData%\NetShutUp\bin\` altında: **uygun**.
- IPv6 DNS v1'de **dışarıda**.

Claude (itiraz edilebilir):
- Önbellek v1'de yok. IPv4 DNS. Sağlayıcılar IP ile adreslenir.
- Sağlayıcı sırası modern hat ile aynı.
