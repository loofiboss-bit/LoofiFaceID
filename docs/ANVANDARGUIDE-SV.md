# Användarguide — LoofiFace-ID

LoofiFace-ID (KFaceAuth) är ett experiment för en lokal, avgränsad jämförelse i den redan
inloggade användarsessionen. Det aktiverar inte inloggning eller autentisering.

## Hem

Hem visar ett enda rekommenderat nästa steg:

- **Konfigurera kamera** när ingen användbar kamera finns;
- **Skapa ansiktsprofil** när kameran är klar men profilen saknas;
- **Testa igenkänning** när profilen är klar;
- **Åtgärda problemet** när worker, modell, KWallet eller valv behöver åtgärdas.

Integritetsöversikten är alltid synlig: bearbetningen är lokal, fångade bilder
sparas inte och profilnyckeln finns endast i KWallet. Varningen om experimentell
status är en del av flödet och innebär inte att autentisering är aktiverad.

## Konfiguration

1. Uppdatera enheter och starta den privata förhandsvisningen uttryckligen.
   Den första användbara kameran väljs automatiskt; välj en annan om flera
   finns.
2. Välj **Analysera aktuell bildruta** för en uttrycklig YuNet-kontroll.
   Återkopplingen gäller ansiktsantal, inramning och bildkvalitet. Den är inte
   livskontroll, spoof-skydd eller autentisering.
3. Välj **Skapa ansiktsprofil** och fånga exakt ett prov per klick. Tre krävs,
   fem rekommenderas och åtta är det hårda maximumet.
4. Använd **Försök igen** för att kasta det senaste tillfälliga provet,
   **Avbryt** för att rensa den osparade registreringen eller **Slutför och
   spara** för den atomiska krypterade lagringen.
5. När profilen är klar väljer du **Öppna Test**. Det aktiverar inte inloggning
   eller någon systemautentisering.

Registreringen löper ut efter 120 sekunder och avbryts när sidan,
förhandsvisningen, programmet eller KCM blir inaktivt. Inget delvis sparas före
**Slutför och spara**.

### Profilhantering

**Ta bort ansiktsprofil** tar bort en giltig krypterad profil efter bekräftelse.
**Återställ oläsbara data** är en separat destruktiv åtgärd för ett oläsbart valv
och dess KWallet-nyckel; ny registrering krävs därefter. Ingen åtgärd lovar
fysisk radering från SSD, ögonblicksbilder, säkerhetskopior, journaler eller
copy-on-write-lagring.

## Test

Starta den privata förhandsvisningen och välj **Testa aktuell bildruta**.
Bildrutan behandlas en gång mot den krypterade profilen. Sidan kan visa
`Matchning`, `Ingen matchning`, `Tvetydigt` eller ett typat otillgängligt
tillstånd, exempelvis saknad profil, låst KWallet, modellfel, avbrott eller
workerfel.

Poäng och trösklar visas inte. Begäran är hastighetsbegränsad och resultatet kan
rensas uttryckligen. `Matchning` påverkar bara den här sidan: den låser inte
upp, autentiserar inte, auktoriserar inte, anropar inte PAM eller Polkit och
ändrar inte Linux-sessionen.

## Diagnostik

Diagnostik är skrivskyddad. Uppdatering kör begränsade lokala kontroller och
visar worker/runtime, verifierade modeller, kameramängd, KWallet, krypterat
valv/profil och en avgränsad återställningsrekommendation. Den röda
support-rapporten innehåller endast aggregerad status. Secure Boot och
display manager är sekundär miljöinformation och ändrar inte jämförelsen.

## Integritetsgräns

Ansiktsinbäddningar är känsliga biometriska uppgifter. KFaceAuth sparar ingen
fångad bild avsiktligt. Bildrutor, landmärken, inbäddningar, nycklar, poäng,
biometriska sökvägar och stabila kameraidentifierare når inte QML, normala
loggar, CLI, support-rapporter eller exempel. KWallet är den enda
produktionsleverantören av nycklar.

Det finns ingen livskontroll eller presentation-/spoofdetektering. FAR, FRR,
bias, demografiskt beteende, RGB/IR-säkerhet och autentiseringslämplighet är
fortsatt okvalificerade. KWallet-nycklar är inte tillgängliga före inloggning.
