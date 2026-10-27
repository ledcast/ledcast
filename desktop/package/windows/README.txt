LEDCAST Console для Windows x64

Требования:
- Windows 10 или 11, 64-bit;
- Microsoft Edge WebView2 Runtime (обычно уже установлен в Windows 10/11);
- NDI runtime уже включён в архив рядом с LEDCAST-Console.exe;
- компьютер и контроллеры LEDCAST в одной локальной сети;
- разрешить LEDCAST Console доступ к частным сетям в Windows Firewall.

Microsoft Visual C++ Redistributable не требуется: CRT встроен в приложение.

Сеть:
- HTTP TCP 80 до контроллеров;
- LEDCAST video UDP 4048 до контроллеров;
- mDNS UDP 5353 для автоматического обнаружения;
- NDI Discovery и NDI video в локальной сети.

Строки находятся автоматически. Если строки нет в списке, проверьте, что
компьютер и контроллер в одной сети и Windows Firewall пропускает mDNS.
