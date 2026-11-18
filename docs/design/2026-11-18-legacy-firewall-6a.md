# Legacy — Windows Güvenlik Duvarı, temel kademe (6a)

Durum: **kodlandı; gerçek Win7/8 makinede denenmedi** · 2026-11-18
Karşılığı: `main` dalındaki `2026-11-18-firewall-6a.md` (Rust). Legacy farkları aşağıda.

## Özet

- Her profil, Domain / Private / Public ağ türlerinde duvarın **açık** ve gelen
  bağlantıların **varsayılan engelli** olmasını sağlar. Windows varsayılanlarıyla
  aynı olduğu için çoğu makinede **hiçbir şey değişmez**; değer, biri duvarı
  kapatmış ya da gelen varsayılanı "izin ver" yapmışsa ortaya çıkar.
- Üçüncü taraf güvenlik duvarı kayıtlıysa bu ayarlar **atlanır**, nedeni
  söylenir (önizleme ve apply'da `Note:` satırı).

## Legacy tasarımı

- 6 katalog girdisi: `firewall.<domain|private|public>.<enabled|inbound>`,
  `Scope::FIREWALL` (yeni). `hive = nullptr`, `subkey` = profil adı,
  `value_name` = `enabled` / `inbound`.
- `store.h/.cpp`: tek okuma/yazma/silme yolu. GLOBAL ve PER_ADAPTER registry'ye,
  FIREWALL `firewall.h`'ye gider. `backup`, `apply`, `preview`, `scan`, `selftest`
  artık registry'yi doğrudan değil `store` üzerinden çağırır.
- `firewall.h/.cpp`: `INetFwPolicy2` (Vista+, Win7 dahil). Yazma sonrası **geri
  okuma**: etkin değer hedef değilse (Grup İlkesi ezmesi) `false` döner ve mevcut
  motor tüm değişiklikleri yedekten geri alır. `netsh` çıktısı ayrıştırılmaz.
- Hedef zaten ayarlıysa apply yazmaz.
- Güvenlik duvarında "ayarlanmamış" yoktur; yedek etkin değeri tutar, geri alma o
  değeri yazar (`store::remove` FIREWALL için işlem yapmaz).
- Üçüncü taraf tespiti: WMI `ROOT\SecurityCenter2`, `FirewallProduct.displayName`;
  adı "Windows" ile başlamayan ürün üçüncü taraf sayılır. Liste okunamazsa Windows
  duvarı varsayılır. Dile bağlı adlar (ör. Fransızca "Pare-feu Windows") üçüncü
  taraf sayılır: sonuç duvarı **atlamak**, yani güvenli yön.
- Self-test: anlık görüntüye FIREWALL girdileri dahil; güvenlik ağına
  `netsh advfirewall export` (yalnızca çıkış kodu) eklendi.

## Kapsam dışı

6b (Ağ Keşfi / Dosya ve Yazıcı Paylaşımı kural grupları, telemetri dışa bağlanma
kuralı): kural grubu kimlikleri Win7'de doğrulanmadı (`@FirewallAPI.dll,-28502`
Dosya ve Yazıcı Paylaşımı için belgeli; Ağ Keşfi için doğrulanamadı). Sanal makine
testinden sonra.

## Test

`tests/test_firewall.cpp`: üçüncü taraf ad kuralı (saf), 6 girdinin katalog
tutarlılığı, **salt okunur** okuma testi ve geçersiz argümanlar. Hiçbir test
güvenlik duvarına yazmaz. Gerçek yazma denemesi `selftest` ile tester makinesinde.

## Bilinen risk

- Win7'de `ROOT\SecurityCenter2` yok sayılırsa (servis kapalı) tespit boş döner ve
  Windows duvarı varsayılır; üçüncü taraf duvarlı bir makinede ayarlar yazılabilir.
- Win7 / Win8 farkları kaynaklarda doğrulanamadı; ilk gerçek koşu `selftest` ile.
