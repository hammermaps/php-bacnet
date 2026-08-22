# Repository-Anweisungen

## Sprache und Geltungsbereich

Kommunikation, Commit-Betreffzeilen, Pull-Request- und Release-Texte werden
auf Deutsch verfasst. Diese Regeln gelten für die native PHP-Erweiterung
`php-bacnet`; die Anweisungen des separaten HTTP-Proxys unter `/var/www/html`
gelten nicht für dieses Repository.

## Wartungszweige und Plattformen

| Zweig | Unterstützte Plattformen | Thread-Sicherheit | PIE-Release-Tags |
|---|---|---|---|
| `v0.1.x` | Linux | NTS | `v0.1.Z` |
| `v0.2.x` | Linux, Windows | NTS | `v0.2.Z` |
| `v0.3.x` | Linux, Windows | NTS und ZTS | `v0.3.Z` |

Der Zweig `v0.3.x` bleibt bis zur grünen ZTS-Matrix ein unveröffentlichter
Integrationszweig. Seine README muss den tatsächlichen ZTS-Stand nennen; ein
Zielzustand allein ist keine Unterstützung und autorisiert keinen Release.

`main` ist der Integrationszweig für die nächste aktive Entwicklungslinie.
Vor einem Wartungsrelease wird der geprüfte Stand in den passenden
`v0.Major.x`-Zweig übernommen. Existiert der nächste Wartungszweig noch nicht,
wird seine Zielplattform bereits auf `main` eingehalten.

PIE-Versionen und GitHub-Tags verwenden immer exakt das Schema `vX.Y.Z`.
Die zugehörige Erweiterungsversion lautet ohne Präfix `X.Y.Z` und muss
konsistent in `php_bacnet.h`, `package.xml`, dem Changelog und den
versionsführenden Dokumentationsstellen stehen.

## Änderungen und Backports

- Plattformneutrale Fehlerkorrekturen, Sicherheitsänderungen, BACnet-Protokoll-
  korrekturen, Cache-Korrekturen, PHP-API-Erweiterungen, Stubs,
  Dokumentationskorrekturen und zugehörige PHPT-Tests werden in jeden noch
  unterstützten Wartungszweig übernommen, sofern die API- und ABI-Kompatibilität
  des Zweigs dies zulässt.
- Eine Änderung wird zuerst in der ältesten betroffenen Wartungslinie umgesetzt
  oder nach ihrer Umsetzung gezielt in alle anderen betroffenen Linien
  übernommen. Jeder Backport erhält dieselben Tests und angepasste
  Versionshinweise.
- Reine Windows-Änderungen (Windows-Build, `config.w32`, Win32 Shared Memory,
  Windows-PHPTs und Windows-Dokumentation) gehören nur nach `v0.2.x`,
  `v0.3.x` und spätere dafür freigegebene Linien. Sie werden nicht nach
  `v0.1.x` zurückportiert.
- Reine ZTS-Änderungen, TSRM-Synchronisation und ZTS-Artefakte gehören erst
  nach `v0.3.x`. Sie dürfen nicht in `v0.1.x` oder `v0.2.x` übernommen werden.
- Änderungen an `deps/` benötigen eine nachvollziehbare Versions- oder
  Sicherheitsbegründung. Die gepinnte LMDB-Quelle wird auf allen passenden
  Linux-Linien verwendet; Build-Artefakte innerhalb des Submoduls bleiben
  unversioniert.

## Validierung und Releases

Vor einem Commit werden mindestens `git diff --check`,
`./scripts/check-coding-standards.sh`, ein passender Build und die PHPT-Suite
ausgeführt. Plattformabhängige Tests sind in der jeweiligen GitHub-Matrix zu
bestätigen:

- `v0.1.x`: Linux mit PHP 8.4 und 8.5.
- `v0.2.x`: Linux und Windows NTS mit PHP 8.4 und 8.5.
- `v0.3.x`: Linux und Windows jeweils NTS und ZTS mit den unterstützten
  PHP-Versionen.

Ein GitHub-Release wird erst nach grüner Matrix erzeugt. Der Tag zeigt auf den
entsprechenden Wartungszweig, die Release-Notizen sind auf Deutsch und enthalten
echte Markdown-Zeilenumbrüche. Anschließend ist das PIE-Quellartefakt zu
prüfen; seine Versionsnummer muss dem Tag ohne führendes `v` entsprechen.

## Arbeitsbaum und Sicherheit

`config.h` sowie Build- und Testartefakte werden nicht committed. Änderungen
an öffentlichen Stubs und API-Signaturen benötigen eine dokumentierte
Kompatibilitätsprüfung. Fremde, unversionierte Dateien im Arbeitsbaum dürfen
nicht gestaged oder gelöscht werden.
