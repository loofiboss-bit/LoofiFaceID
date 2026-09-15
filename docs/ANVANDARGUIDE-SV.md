# Användarguide — LoofiFace-ID

LoofiFace-ID (KFaceAuth) är ett experimentellt verktyg för en lokal profil och
en uttrycklig jämförelse i den redan inloggade Fedora 44/KDE-sessionen. Det
aktiverar inte PAM, SDDM, sudo, Polkit, upplåsning eller inloggning.

## Installera och starta

På Fedora 44 är COPR den enklaste vägen:

```bash
sudo dnf copr enable loofitheboss/loofifaceid
sudo dnf install kfaceauth
systemsettings kcm_kfaceauth
```

Öppna **Hem** och välj den enda primära åtgärden. Välj **Kom igång** när en
profil saknas och kameran ännu inte är startad. När bara en användbar kamera
finns väljs den automatiskt; kameraväljaren visas först när flera finns.

## Första start och registrering

1. Klicka **Kom igång**. Det är det uttryckliga medgivandet att starta den
   privata kameraförhandsvisningen.
2. Följ samma guide hela vägen: placering, **Frontal**, **Vänster**,
   **Höger**, **Luta** och **Naturlig**.
3. Automatisk fångst kräver exakt ett ansikte, godkänd inramning/bildkvalitet
   och tre nya analyser under minst 600 ms. Stabiliteten nollställs direkt när
   villkoret bryts och en spärr på 800 ms används efter ett godkänt prov.
4. **Fånga manuellt** är alltid reservknappen. Efter tre prov visas **Spara
   nu**; fem prov är rekommenderat och åtta är hårt maximum.
5. Välj **Spara profil** själv. Profilen sparas aldrig automatiskt. Välj sedan
   **Testa profilen** eller öppna fliken **Test**.

Vid avvisat prov fortsätter guiden och visar en konkret instruktion, till
exempel att flytta närmare, centrera ansiktet, förbättra ljuset eller visa
endast ett ansikte. Kamerabilder sparas inte.

## Hem, Test och profilhantering

Hem visar **Kom igång**, **Fortsätt registreringen**, **Testa profilen** eller
**Åtgärda problem** beroende på aktuellt tillstånd. **Test** behandlar en enda
avsiktlig bildruta mot den krypterade lokala profilen; resultatet påverkar inte
Linux-sessionen.

**Ta bort ansiktsprofil** och **Återställ oläsbara data** är separata,
bekräftade åtgärder. De lovar inte fysisk radering från SSD, ögonblicksbilder,
säkerhetskopior, journaler eller copy-on-write-lagring.

## Diagnostik och fel

Diagnostik är skrivskyddad och ändrar inte andra kameratjänster eller
systemkonfiguration. Uppdatera och följ den typade återställningen för:

- upptagen eller frånkopplad kamera;
- worker-, protokoll- eller modellfel;
- låst/otillgängligt KWallet;
- oläsbar eller modellinkompatibel profil;
- ej kvalificerad distribution/version.

Den röda rapporten är avgränsad och redigerad. Skicka inte kamerabilder,
inbäddningar, nycklar eller lösenord när du rapporterar ett fel.

## Integritetsgräns

Råa landmärken, detektorsiffror, poäng, bilder och inbäddningar stannar i de
privata worker-/backendlagren och exponeras inte i QML, loggar eller rapporter.
Det finns inga reproducerbara PAD-, prestanda-, bias- eller
autentiseringskvalificeringar i denna version.
