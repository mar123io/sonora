# Diario di bordo

Una riga a settimana. A dicembre questo file è il materiale grezzo per il
writeup e per le risposte in colloquio: cosa ho sbagliato, cosa mi ha sorpreso,
quanto tempo è costato davvero rispetto alla stima.

Formato: cosa ho fatto · cosa è costato più del previsto · cosa ho imparato.

---

## Settimana 1 — Scheletro buildabile (21-27 set 2026)

**Obiettivo:** `cmake --preset win-debug && cmake --build --preset win-debug`
produce `Sonora.exe`, si apre una finestra ridimensionabile, `ctest` è verde.

- [ ] Configure e build su macchina pulita
- [ ] La finestra si apre, si ridimensiona, rispetta la dimensione minima
- [ ] Spostandola tra due monitor a DPI diversi non "salta"
- [ ] `ctest --preset win-debug` verde
- [ ] CI verde su tutti e tre i runner
- [ ] `git tag v0.1-shell`

**Note:**

_(da compilare a fine settimana)_
