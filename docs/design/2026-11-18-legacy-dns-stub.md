# Legacy — Şifreli DNS (yerel DoH çözücü)

Durum: **taslak, Arda'nın onayını bekliyor; kod yok** · 2026-11-18
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
  çözücü ister. Bu yüzden v1 yalnızca **DoH**.
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

- DoT, DNSSEC doğrulama, DNS filtreleme, kullanıcı tanımlı sağlayıcı, enterprise sağlayıcılar.
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

### Çalışma sırasında hata

- Servis her sorguyu sırayla üst sunuculara dener. **Varsayılan (Arda'ya soruluyor):**
  tüm DoH üst sunucuları ardışık N kez başarısızsa servis, kayıtlı **eski DNS'e** geçer
  (internet kesilmez), bunu `%ProgramData%\NetShutUp\dns.status` dosyasına yazar ve DoH
  yeniden çalışınca şifreliye döner. Alternatif: şifreli kalıp çözmeyi reddetmek (daha gizli,
  ama ağ DoH'u engellerse internet gider).
- `netshutup-legacy dns-status`: servis çalışıyor mu, şu an şifreli mi yoksa eski DNS'e mi
  düşmüş, son hata, hangi sağlayıcı.
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
| 7a | `dns_core` + `dns.cfg` + testler | hayır |
| 7b | `dns_doh` (WinHTTP) + sahte testleri | yalnızca derleme |
| 7c | `dns_server` + `dns_service` (`--dns-service`) | döngü adresi testi |
| 7d | `dns_setup`, `dns_adapters`, yan dosya yedek, `dns_apply`, önizleme/menü/CLI | **VM gerekir** |
| 7e | selftest entegrasyonu, belgeler, `dns-status` | VM |

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

## Claude'un kararları (itiraz edilebilir)

- Yalnızca DoH; DoT yok. Önbellek yok. IPv4 DNS.
- `.exe` kopyası `%ProgramData%\NetShutUp\bin\` altında (indirilen `.exe` silinirse DNS bozulmasın).
- Sağlayıcılar IP ile; sağlayıcı sırası modern hat ile aynı.
- Çalışma sırasında hata ilkesi: **eski DNS'e geç, durumu göster** (internet kesilmesin).

## Açık sorular (Arda)

1. Çalışma sırasında DoH tamamen engellenirse: **internet çalışsın diye eski DNS'e düşelim**
   (önerilen) mı, yoksa **şifresiz sorgu asla gitmesin, internet gitse de** mi?
2. `.exe`'nin `%ProgramData%` altına kopyalanması kabul mü?
3. IPv6 DNS'i v1'de dışarıda bırakmak kabul mü (modern hat gibi)?
