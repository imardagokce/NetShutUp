# NetShutUp nasıl test edilir (adım adım)

*[English version](TESTING.md)*

Yardım ettiğiniz için teşekkürler. **Yazılımcı olmanız gerekmez.** Bu rehber yaklaşık **10–15 dakika** sürer; neye tıklayacağınızı, ne göreceğinizi ve bir şey ters giderse ne yapacağınızı anlatır.

## Gerekenler

- Windows 10 ya da Windows 11 bilgisayar (Windows 11'de şifreli DNS de test edilir).
- Yönetici hesabı (Windows normal bir izin penceresi gösterir).
- Mümkünse **kendi bilgisayarınız**; iş ya da okul bilgisayarları yönetilebildiği için farklı davranabilir (yine de denerseniz raporda belirtin).
- İnternete ihtiyacınız olmayan birkaç dakika. Test sırasında internet bir an kopabilir.

## Güvenli mi?

Test bazı ağ ayarlarını değiştirir, **kontrol eder ve yaptığı her değişikliği geri alır**. Sizi korumak için:

- Bir şeyi değiştirmeden önce güvenlik kopyaları alır: ilgili kayıt defteri anahtarlarının dışa aktarımı ve bir Windows **Sistem Geri Yükleme noktası**.
- Her değişiklik önce yedeklenir ve menüden tek seçimle geri alınabilir.
- Bir şey geri yüklenemezse program bunu açıkça söyler ve nasıl düzelteceğinizi gösterir ([Bir şey ters giderse](#bir-şey-ters-giderse)).
- **İnternete hiçbir şey gönderilmez.** Yalnızca bilgisayarınızda bir rapor dosyası yazar; paylaşıp paylaşmamaya siz karar verirsiniz.

Hiçbir şeyi değiştirmek istemezseniz yalnızca [salt okunur incelemeyi](#3-adım--isteğe-bağlı-salt-okunur-bakış) yapıp gördüklerinizi gönderebilirsiniz.

---

## 1. Adım — İndirin ve dosyayı kontrol edin

1. [Son sürüm sayfasından](https://github.com/imardagokce/NetShutUp/releases) şunları indirin:
   - `NetShutUp-<sürüm>-windows-x64.zip`
   - `NetShutUp-<sürüm>-windows-x64.zip.sha256`
2. *(Önerilir)* İndirmenin bozulmadığını kontrol edin. Dosyaları kaydettiğiniz klasörde **PowerShell** açıp şunları çalıştırın (dosya adını kendinizinkiyle değiştirin):

   ```powershell
   (Get-FileHash .\NetShutUp-v0.1.0-alpha.1-windows-x64.zip -Algorithm SHA256).Hash.ToLower()
   Get-Content .\NetShutUp-v0.1.0-alpha.1-windows-x64.zip.sha256
   ```

   İlk komutun yazdığı uzun metin, ikinci komutun çıktısının başıyla aynı olmalı. Farklıysa yeniden indirin.
3. Zip'e sağ tıklayın → **Tümünü ayıkla…** ve bir klasör seçin (örneğin Belgeler).

## 2. Adım — Programı başlatın

1. Ayıkladığınız klasörü açıp **`netshutup-modern.exe`**'ye çift tıklayın.
2. Windows mavi bir kutuyla **"Windows bilgisayarınızı korudu"** diyebilir. Bu beklenen bir durumdur: program yeni ve henüz imzalı değil. 1. Adım'daki sağlama eşleştiyse **Ek bilgi → Yine de çalıştır**'a tıklayın.
3. Bir terminal penceresi ve menü açılır:

   ```text
   NetShutUp 0.1.0-alpha.1 · Windows 11 (build 26200) · not elevated

   ? What would you like to do?
   > Scan current settings
     Apply a profile
     Revert last change
     Self-test (for testers)
     Exit
   ```

   Hareket için **↑ ↓ ok tuşları**, seçmek için **Enter**, geri gitmek için **Esc**.

   *Terminalinizde düzgün görünüyor mu (renkler, ok tuşları)? Değilse raporda belirtin.*

## 3. Adım — (İsteğe bağlı) Salt okunur bakış

**Scan current settings**'i seçin. Ağ bağdaştırıcılarınızı ve NetShutUp'ın bildiği her ayarın şu anki değerini listeler. Hiçbir şey değişmez, yönetici yetkisi gerekmez. Menüye dönmek için bir ok tuşuna basın.

Bir profilin ne yapacağını yapmadan görmek için **Apply a profile → Privacy**'yi (ya da başka birini) seçin. Yönetici izni isterse *Hayır* diyebilirsiniz; bu yalnızca önizlemedir. Normal kullanımda Privacy profili ayrıca isteğe bağlı güvenlik duvarı soruları sorabilir (her birinde varsayılan Hayır); self-test bunlara evet der ve geri alır.

## 4. Adım — Self-test'i çalıştırın

1. Menüde **Self-test (for testers)**'i seçin.
2. Windows yönetici olarak çalıştırma izni ister; **Evet**'e tıklayın. **Yeni bir pencere** açılır ve orada devam eder.
3. Kısa açıklamayı okuyun. Ne olacağını ve rapora hangi bilgilerin gireceğini söyler. Varsayılan cevap **No**'dur; devam etmek için `y` yazıp Enter'a basın.
4. Bekleyin. Program her adım için bir satır yazar. Sırasıyla:
   1. bilgisayarınız hakkında temel bilgileri toplar (isim, parola yok),
   2. güvenlik kopyalarını alır (kayıt defteri dışa aktarımı ve geri yükleme noktası),
   3. her ayarın okunabildiğini kontrol eder,
   4. her profil için (**Privacy**, **Performance**, **Recommended**): uygular, kontrol eder, şifreli DNS'i dener, **geri alır** ve her şeyin eskisi gibi olduğunu doğrular,
   5. raporu yazar.

   Birkaç dakika sürer. İnternet bir iki saniye kopabilir. Rapor yazıldı denene kadar **pencereyi kapatmayın**.
5. Sonunda şuna benzer bir çıktı verir:

   ```text
   Self-test finished: 38 passed, 0 failed, 3 skipped, 6 notes.

   Report written:
     C:\ProgramData\NetShutUp\reports\NetShutUp-selftest-20261118-120000.md
     ...
   ```

   ve raporun bulunduğu klasörü açar.

   Bunun yerine **"NOT everything was restored"** görürseniz doğrudan [Bir şey ters giderse](#bir-şey-ters-giderse) bölümüne gidin.

## 5. Adım — Raporu inceleyin ve gönderin

1. `.md` dosyasını açın (Not Defteri ya da herhangi bir metin/markdown görüntüleyiciyle açılır). En üstte özet (✅ geçti, ❌ kaldı, ⏭ atlandı, ℹ️ not) ve **"Everything restored: yes"** görürsünüz.
2. Göz gezdirin. İçinde kullanıcı adı, bilgisayar adı, Wi-Fi adı ya da tam IP/MAC adresi **olmamalı** (adresler `192.168.x.x` gibi maskelenir). Sorunları anlamamıza yardım eden donanım ve ağ bilgileri vardır; **"What this report contains"** bölümü bunları sayar.
3. **[Yeni test raporu](https://github.com/imardagokce/NetShutUp/issues/new?template=test-report.yml)** sayfasına gidin (ücretsiz bir GitHub hesabı gerekir; istemezseniz iki dosyayı test etmenizi isteyen kişiye gönderin).
4. Formu doldurun: Windows sürümü (raporda ve menü başlığında yazar), ne çalıştırdığınız, sonuç.
5. **İki dosyayı da ekleyin**: `.md` ve `.json` (yorum kutusuna sürükleyin).
6. Sizi şaşırtan ya da kafa karıştıran her şeyi ekleyin. "Her şey çalıştı" bile değerli bir rapordur!

---

## Bir şey ters giderse

Sırayla deneyin; düzelir düzelmez durun.

1. **Programın kendi geri almasını kullanın.** `netshutup-modern.exe`'yi başlatın, **Revert last change**'i seçin, yönetici iznini verin, onaylayın. Her şeyi yedekten geri koyar. (Menünün üstündeki "A previous self-test did not leave your settings as they were" uyarısı da bunu yapmanız gerektiği anlamına gelir.)
2. **İnternet çalışmıyor mu?** Bilgisayarı yeniden başlatın, sonra yukarıdaki adımı yeniden deneyin.
3. **Hâlâ sorun varsa** testin oluşturduğu Sistem Geri Yükleme noktasını kullanın:
   1. **Başlat**'a basın, **Geri yükleme noktası oluştur** yazıp açın.
   2. **Sistem Geri Yükleme…**'ye, sonra **İleri**'ye tıklayın.
   3. **"NetShutUp self-test"** adlı noktayı seçip sihirbazı bitirin.
   *(Böyle bir nokta yoksa Sistem Koruması kapalıydı ya da son 24 saatte bir nokta oluşturulmuştu; rapor hangisi olduğunu söyler.)*
4. **Son çare:** test ayrıca kayıt defteri dışa aktarımlarını (`.reg` dosyaları) `C:\ProgramData\NetShutUp\reg-backups\` içine kaydeder. Bir dosyaya çift tıklamak kayıtlı değerleri geri birleştirir. Bunu yalnızca rahat ediyorsanız yapın ya da önce bize sorun.
5. **Sonra raporu yine de gönderin.** Başarısızlık, gönderebileceğiniz en değerli şeydir.

## Sorular

**Sonra programı tutmam gerekir mi?** Hayır. Ayıkladığınız klasörü silin. Raporlar `C:\ProgramData\NetShutUp\reports` (ve Masaüstünüzde), yedekler `C:\ProgramData\NetShutUp\backups` içinde kalır; işiniz bitince silebilirsiniz.

**Antivirüsüm uyarı veriyor.** Program yeni ve imzasız, ayrıca sistem ayarlarını değiştirmek üzere tasarlandı; bu şüpheli görünebilir. Sağlamayı kontrol edin (1. Adım). Rahat değilseniz kaynaktan derleyin (README'ye bakın) ya da testi atlayın.

**VPN, proxy, kurumsal yönetilen bilgisayar ya da üçüncü taraf güvenlik duvarı kullanıyorum.** Yine test edebilirsiniz ama formda belirtin; bunlar sonuçları değiştirir (örneğin VPN, şifreli DNS'in çalışıp çalışmadığını gizleyebilir).

**"Encrypted DNS did not work on this network" diyor.** Bu mutlaka bir hata değildir: bazı ağlar (ofis, okul, bazı hotspot'lar) bunu engeller ve program eski DNS'inizi geri koyar. Hangi tür ağda olduğunuzu söyleyin.

**DNS'ime ne yapıyor?** Ethernet/Wi-Fi bağdaştırıcılarınızı gizlilik dostu bir sağlayıcıya (Quad9, Cloudflare ya da Google) şifreli DNS ile yönlendirir, gerçek sorgularla dener, çalışmazsa eski DNS'inizi geri koyar. Self-test sırasında her şey sonunda geri alınır.

**Windows 10'da test edebilir miyim?** Evet. Şifreli DNS dışındaki her şey test edilir; o kısım Windows 10'da atlanır.

**Birden fazla çalıştırabilir miyim?** Evet. Her çalıştırma bağımsızdır.

**Yardımı nereden alırım?** GitHub'da bir issue açın ya da size test etmenizi söyleyen kişiye yazın.

## Bize söylemeniz çok faydalı olur

- Menü doğru görünüyor ve tepki veriyor mu (renkler, ok tuşları, Esc)?
- Testten sonra internetiniz ve DNS'iniz eskisi gibi mi (Ayarlar → Ağ ve internet → bağlantınız → DNS sunucusu ataması)?
- Hangi ağdaydınız (ev, mobil hotspot, ofis)?
- Anlatımda ya da adımlarda kafa karıştıran bir şey oldu mu?

Teşekkürler! Katkı hakkında daha fazlası: [`CONTRIBUTING.md`](../CONTRIBUTING.md).
