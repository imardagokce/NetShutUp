# ADR-001: Legacy Hat — Dil ve Çalışma Zamanı Seçimi

- **Durum:** Kabul edildi
- **Tarih:** 2026-11-18
- **Karar veren:** Arda (proje sahibi)

---

## Bağlam

NetShutUp Legacy hattı Windows 7 SP1, Windows 8 ve Windows 8.1'i hedeflemektedir.
Bu platformlarda ağ ayarlarını yönetmek için registry, Windows Hizmetleri (SCM),
güvenlik duvarı (`INetFwPolicy2`) ve sürüm algılama (`RtlGetVersion`) gibi Win32
API'lerine doğrudan erişim gerekmektedir.

Değerlendirilen kısıtlar:

- Windows 7 SP1 x86 ve x64'te çalışmalı
- Tek `.exe`, harici installer veya runtime kurulumu yok
- Dile bağımlı program çıktısı ayrıştırılmayacak (Türkçe Windows uyumu)
- Katalog ve profiller binary'ye gömülü
- Yönetici yükseltmesi UAC üzerinden, yetkilendirme olmadan tarama mümkün

---

## Değerlendirilen Seçenekler

| Seçenek | Win7 Desteği | Sıfır Runtime | Win32 API | Not |
|---|---|---|---|---|
| **C++ / Win32 (MSVC veya MinGW)** | ✅ | ✅ | ✅ Doğrudan | Seçilen |
| C# / .NET Framework 4.x | ✅ | ❌ | P/Invoke ile | .NET 4.x kullanıcıda kurulu olmayabilir |
| Go (≤ 1.10) | ✅ | ✅ | Wrapper ile | Win32 erişimi dolaylı, ekosistem kısıtlı |
| Rust (eski toolchain) | ⚠️ Karmaşık | ✅ | ✅ | Modern Rust Win7 desteğini bıraktı; eski toolchain riski yüksek |
| Zig / Carbon / diğer modern | ❌ / ⚠️ | Değişken | Değişken | Win7 için olgunlaşmamış veya doğrulanmamış destek |

---

## Karar

**Native C++ (C++17) + Win32 API**, tek compiler hedefi olarak seçilmiştir.

- Compiler: **MSVC** (Visual Studio Build Tools) — birincil
- Alternatif: **MinGW-w64** (CI veya cross-build için değerlendirilebilir)
- C++ standardı: **C++20** (MSVC 2019+ ile Win7 binary'si üretilebilir; bkz. not)
- Çalışma zamanı: **Windows Runtime yok**, yalnızca Win32 API ve CRT

> **C++20 ve Win7 uyumluluk notu:** C++20'nin dil özellikleri (concepts, ranges,
> `std::span`, vb.) Win7 ile uyumludur. Ancak aşağıdaki STL primitifleri dahili
> olarak `WaitOnAddress` API'sini (Win8+) kullanır ve Win7'de çalışmaz; bu
> nedenle **proje genelinde yasaktırlar:**
> `std::atomic_wait`, `std::latch`, `std::barrier`, `std::counting_semaphore`.
> Bu kısıtlama code review ve CI ile korunur.

---

## Gerekçe

1. **Sıfır bağımlılık:** Kullanıcı makinesinde hiçbir şeyin kurulu olması gerekmez.
2. **Doğrudan Win32 erişimi:** Registry, SCM, `INetFwPolicy2`, `RtlGetVersion`
   için ek soyutlama katmanı gerekmez.
3. **Kanıtlanmış Win7 uyumluluğu:** MSVC ile üretilen binary'ler Win7 SP1'de
   sorunsuz çalışır; bu durum CI'da doğrulanabilir.
4. **`compatibility-matrix.md` ile örtüşme:** Mevcut belgede zaten
   "Native C++ / Win32" önerisi yer alıyordu.

---

## Sonuçlar ve Kabul Edilen Riskler

- Modern, "revolutionary" bir dil kullanma fırsatından vazgeçildi; bu bilinçli
  bir karardır — legacy hedefi güvenilirliği önceliklendirir.
- C++17 kullanılması Win7'de test gerektiren belirli STL özelliklerini dışarıda
  bırakabilir; risk `docs/compatibility-matrix.md`'de izlenecek.
- x86 desteği henüz kesinleşmedi; bu karar ayrı bir ADR'de ele alınacak
  (bkz. `compatibility-matrix.md` madde 1).

---

## İlgili Belgeler

- [`docs/compatibility-matrix.md`](../compatibility-matrix.md)
- [`GEMINI.md`](../../GEMINI.md) — Legacy'ye özgü dikkat noktaları
