# mein-bazzite

🎮 vDS Treiber & Daemon – Nachbereitung & StartDiese Anleitung beschreibt, was du nach dem erfolgreichen Installieren und Booten deines personalisierten Bazzite-Images tun musst, um deinen vDS-Controller in Betrieb zu nehmen.

🛠️ Schritt 1: System-Check auf dem Host (Bazzite)Durch unser neues Rezept wird das Kernel-Modul nun ab dem Systemstart automatisch geladen und die Bluetooth-Rechte werden von selbst vergeben. Überprüfe das kurz auf deinem normalen Bazzite-Terminal (nicht in der Distrobox):Kernel-Modul prüfen:bashlsmod | grep vds
Verwende Code mit Vorsicht.Hier sollte dir nun direkt eine Zeile mit vds_hcd angezeigt werden.Bluetooth SDP-Socket prüfen:bashls -l /run/sdp
Verwende Code mit Vorsicht.Der Socket muss existieren und am Anfang die Rechte srwxrwxrwx (777) aufweisen.

📦 Schritt 2: vDS-Daemon in der Distrobox startenDa wir hardwarenahe Bluetooth-Ports und Sockets nutzen, führen wir den Daemon in der dafür vorbereiteten Root-Distrobox aus.Öffne deine Root-Distrobox:bashdistrobox enter --root ubuntu-apps
Verwende Code mit Vorsicht.Wechsle in deinen vDS-Ordner:(Passe den Pfad an den Ort an, an dem deine vdsd-Datei liegt)bashcd /var/home/ksanto/bin
Verwende Code mit Vorsicht.Starte den Daemon mit Root-Rechten:bashsudo ./vdsd
Verwende Code mit Vorsicht.Wichtig: Der Cursor springt in die nächste Zeile und bleibt dort stehen. Das ist richtig so! Das Terminal lauscht jetzt aktiv auf deinen Controller. Lass dieses Fenster einfach im Hintergrund offen.*🔗 Schritt 3: Controller verbindenVersetze deinen Controller in den Pairing-Modus (z. B. beim DualSense: Create/Share-Taste + PS-Taste gedrückt halten, bis er schnell blinkt).Öffne das ganz normale Bluetooth-Menü von Bazzite (entweder im Steam-Gaming-Modus oder in den KDE/GNOME-Desktop-Einstellungen).Klicke auf Verbinden.

🎉 Was jetzt passiert:Sobald die Bluetooth-Verbindung steht, leitet Bazzite die Daten durch den freigegebenen Socket direkt an deine Distrobox weiter. In deinem blockierten vdsd-Terminal siehst du nun sofort die Log-Ausgaben der erfolgreichen Verbindung – und der Controller bleibt dauerhaft stabil verbunden!
