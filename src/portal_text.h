#pragma once
// Player-visible notices in every language Enshrouded supports. This file is UTF-8; build with
// /utf-8 so the literals stay UTF-8. The language follows the game's own setting, detected from
// its word for "Legendary" (same approach as Embervale Teleport and Find).
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace portal::text {

enum class Lang : uint8_t { en, fr, it, de, es, ja, ko, pl, pt, ru, zhHans, zhHant, tr, uk, th, count };
inline constexpr const char* kCodes[]={"en","fr","it","de","es","ja","ko","pl","pt","ru","zh-hans","zh-hant","tr","uk","th"};

enum Id { StillChecking, ServerMissing, ChannelCancelled, Travelling, ClosesSoon, TravelTo, ReturnTo, SharedTo, TravellingTo, OpeningPlace, YourAltar, NoRoom, NoPortalShroud, NoPortalHere,
          Nag5, Nag10, Nag15, // Nag*: the 5th, 10th and 15th refusal in a row (tools/voice/set/lines_nag.tsv)
          LoadingPercent,     // the loading screen's progress, printf with the percent (0-100)
          PortalClosed, NoOpenPortal, // holding the cast key closes your portal
          HoldToClose,        // the prompt row under your own portal's prompt
          IdCount };

inline constexpr const char* kTexts[size_t(Lang::count)][IdCount]={
// en
{"Still checking whether this server runs Town Portal. Try again in a few seconds.",
 "This server does not run Town Portal, so portals are unavailable here. The server needs the mod installed too.",
 "Portal cancelled.",
 "Travelling through the portal...",
 "Your portal closes in %d seconds, so others can follow you back.",
 "Travel to %s",
 "Return to %s  /  Closes your portal",
 "Travel to %s (shared portal)",
 "Travelling to %s...",
 "where the portal opened",
 "your Flame Altar",
 "No room for a portal here: a wall is too close.",
 "Portals can't be opened in the Shroud.",
 "%s: portals can't be opened here.",
 "You keep asking. The answer is the same: no portal.",
 "Yup, that's the portal button. You keep pressing it. Here's your gold star.",
 "This is why you're single, isn't it?",
 "Loading… %d%%",
 "Portal closed.",
 "You have no open portal.",
 "Hold: Close your portal"},
// fr
{"Vérification en cours : ce serveur utilise-t-il Town Portal ? Réessayez dans quelques secondes.",
 "Ce serveur n'utilise pas Town Portal : les portails ne sont pas disponibles ici. Le mod doit aussi être installé sur le serveur.",
 "Portail annulé.",
 "Passage par le portail...",
 "Votre portail se ferme dans %d secondes, pour que les autres puissent vous suivre.",
 "Voyager vers %s",
 "Retourner à %s  /  Ferme votre portail",
 "Voyager vers %s (portail partagé)",
 "Voyage vers %s...",
 "l'ouverture du portail",
 "votre Autel de la Flamme",
 "Pas de place pour un portail ici : un mur est trop proche.",
 "Impossible d'ouvrir un portail dans la brume.",
 "%s : impossible d'ouvrir un portail ici.",
 "Tu insistes, mais la réponse reste la même : pas de portail.",
 "Oui, c'est bien le bouton du portail. Tu n'arrêtes pas d'appuyer dessus. Tiens, voilà ton bon point.",
 "C'est pour ça que tu es célibataire, hein ?",
 "Chargement… %d %%",
 "Portail fermé.",
 "Vous n'avez aucun portail ouvert.",
 "Maintenir : fermer votre portail"},
// it
{"Verifica in corso: questo server usa Town Portal? Riprova tra qualche secondo.",
 "Questo server non usa Town Portal, quindi i portali non sono disponibili qui. Anche il server deve avere la mod installata.",
 "Portale annullato.",
 "Attraversamento del portale...",
 "Il tuo portale si chiude tra %d secondi, così gli altri possono seguirti.",
 "Viaggia verso %s",
 "Torna a %s  /  Chiude il tuo portale",
 "Viaggia verso %s (portale condiviso)",
 "Viaggio verso %s...",
 "il punto di apertura",
 "il tuo Altare della Fiamma",
 "Non c'è spazio per un portale qui: un muro è troppo vicino.",
 "Non si possono aprire portali nella coltre.",
 "%s: qui non si possono aprire portali.",
 "Continui a chiedere. La risposta è sempre la stessa: niente portale.",
 "Sì, quello è il tasto del portale. Continui a premerlo. Ecco la tua stellina d'oro.",
 "È per questo che sei single, vero?",
 "Caricamento… %d%%",
 "Portale chiuso.",
 "Non hai nessun portale aperto.",
 "Tieni premuto: chiudi il portale"},
// de
{"Es wird noch geprüft, ob dieser Server Town Portal verwendet. Versuche es in ein paar Sekunden erneut.",
 "Dieser Server verwendet Town Portal nicht, daher sind Portale hier nicht verfügbar. Die Mod muss auch auf dem Server installiert sein.",
 "Portal abgebrochen.",
 "Reise durch das Portal ...",
 "Dein Portal schließt sich in %d Sekunden, damit andere dir folgen können.",
 "Reise nach %s",
 "Zurück nach %s  /  Schließt dein Portal",
 "Reise nach %s (geteiltes Portal)",
 "Reise nach %s ...",
 "den Öffnungsort",
 "deinen Flammenaltar",
 "Hier ist kein Platz für ein Portal: Eine Wand ist zu nah.",
 "Im Nebel können keine Portale geöffnet werden.",
 "%s: Hier können keine Portale geöffnet werden.",
 "Du fragst immer wieder. Die Antwort bleibt dieselbe: kein Portal.",
 "Jep, das ist der Portalknopf. Du drückst ihn immer wieder. Hier, dein Fleißsternchen.",
 "Deshalb bist du Single, oder?",
 "Wird geladen … %d %%",
 "Portal geschlossen.",
 "Du hast kein offenes Portal.",
 "Halten: Portal schließen"},
// es
{"Comprobando si este servidor usa Town Portal. Inténtalo de nuevo en unos segundos.",
 "Este servidor no usa Town Portal, así que los portales no están disponibles aquí. El servidor también necesita tener el mod instalado.",
 "Portal cancelado.",
 "Atravesando el portal...",
 "Tu portal se cierra en %d segundos para que otros puedan seguirte.",
 "Viajar a %s",
 "Volver a %s  /  Cierra tu portal",
 "Viajar a %s (portal compartido)",
 "Viajando a %s...",
 "el lugar de apertura",
 "tu Altar de la Llama",
 "No hay espacio para un portal aquí: hay una pared demasiado cerca.",
 "No se pueden abrir portales en la bruma.",
 "%s: aquí no se pueden abrir portales.",
 "Sigues preguntando. La respuesta es la misma: no hay portal.",
 "Sí, ese es el botón del portal. No paras de pulsarlo. Toma tu estrellita dorada.",
 "Por esto no tienes pareja, ¿verdad?",
 "Cargando… %d %%",
 "Portal cerrado.",
 "No tienes ningún portal abierto.",
 "Mantener: cerrar tu portal"},
// ja
{"このサーバーでTown Portalが動作しているか確認中です。数秒後にもう一度お試しください。",
 "このサーバーではTown Portalが動作していないため、ここではポータルを使用できません。サーバーにもMODのインストールが必要です。",
 "ポータルをキャンセルしました。",
 "ポータルを通過中...",
 "他のプレイヤーが戻れるよう、ポータルは%d秒後に閉じます。",
 "%sへ移動",
 "%sへ戻る  /  ポータルが閉じます",
 "%sへ移動（共有ポータル）",
 "%sへ移動中...",
 "ポータルを開いた場所",
 "あなたの炎の祭壇",
 "ここにはポータルを開く場所がありません。壁が近すぎます。",
 "シュラウドの中ではポータルを開けません。",
 "%s：ここではポータルを開けません。",
 "何度頼んでも答えは同じ。ポータルは無理。",
 "うん、それがポータルのボタンだよ。押し続けてるね。はい、金色の星をどうぞ。",
 "だから恋人がいないんじゃない？",
 "読み込み中… %d%%",
 "ポータルを閉じました。",
 "開いているポータルはありません。",
 "長押し：ポータルを閉じる"},
// ko
{"이 서버에서 Town Portal을 사용하는지 확인하는 중입니다. 몇 초 후에 다시 시도하세요.",
 "이 서버는 Town Portal을 사용하지 않으므로 여기서는 포털을 사용할 수 없습니다. 서버에도 모드가 설치되어 있어야 합니다.",
 "포털이 취소되었습니다.",
 "포털을 통과하는 중...",
 "다른 플레이어가 따라올 수 있도록 포털이 %d초 후에 닫힙니다.",
 "%s(으)로 이동",
 "%s(으)로 돌아가기  /  포털이 닫힙니다",
 "%s(으)로 이동 (공유 포털)",
 "%s(으)로 이동 중...",
 "포털을 연 곳",
 "당신의 불꽃 제단",
 "여기에는 포털을 열 공간이 없습니다. 벽이 너무 가깝습니다.",
 "장막 안에서는 포털을 열 수 없습니다.",
 "%s: 여기서는 포털을 열 수 없습니다.",
 "계속 물어봐도 대답은 같아. 포털은 안 돼.",
 "그래, 그게 포털 버튼이야. 계속 누르네. 자, 칭찬 스티커.",
 "이래서 애인이 없는 거지?",
 "불러오는 중… %d%%",
 "포털을 닫았습니다.",
 "열려 있는 포털이 없습니다.",
 "길게 누르기: 포털 닫기"},
// pl
{"Trwa sprawdzanie, czy ten serwer używa Town Portal. Spróbuj ponownie za kilka sekund.",
 "Ten serwer nie używa Town Portal, więc portale są tu niedostępne. Mod musi być zainstalowany także na serwerze.",
 "Portal anulowany.",
 "Przechodzenie przez portal...",
 "Twój portal zamknie się za %d s, aby inni mogli za tobą wrócić.",
 "Podróżuj do: %s",
 "Wróć do: %s  /  Zamyka twój portal",
 "Podróżuj do: %s (wspólny portal)",
 "Podróż do: %s...",
 "miejsce otwarcia portalu",
 "twój Ołtarz Płomienia",
 "Brak miejsca na portal: ściana jest zbyt blisko.",
 "We mgle nie można otwierać portali.",
 "%s: tutaj nie można otwierać portali.",
 "Ciągle pytasz. Odpowiedź jest ta sama: żadnego portalu.",
 "Tak, to przycisk portalu. Ciągle go wciskasz. Masz tu złotą gwiazdkę.",
 "To dlatego nikt się z tobą nie umawia, prawda?",
 "Wczytywanie… %d%%",
 "Portal zamknięty.",
 "Nie masz otwartego portalu.",
 "Przytrzymaj: zamknij portal"},
// pt
{"Verificando se este servidor usa o Town Portal. Tente novamente em alguns segundos.",
 "Este servidor não usa o Town Portal, então os portais não estão disponíveis aqui. O servidor também precisa ter o mod instalado.",
 "Portal cancelado.",
 "Atravessando o portal...",
 "Seu portal fecha em %d segundos para que outros possam te seguir.",
 "Viajar para %s",
 "Voltar para %s  /  Fecha seu portal",
 "Viajar para %s (portal compartilhado)",
 "Viajando para %s...",
 "o local de abertura",
 "seu Altar da Chama",
 "Não há espaço para um portal aqui: há uma parede perto demais.",
 "Não é possível abrir portais na bruma.",
 "%s: não é possível abrir portais aqui.",
 "Você continua pedindo. A resposta é a mesma: nada de portal.",
 "Sim, esse é o botão do portal. Você não para de apertar. Aqui está sua estrelinha dourada.",
 "É por isso que ninguém te namora, né?",
 "Carregando… %d%%",
 "Portal fechado.",
 "Você não tem nenhum portal aberto.",
 "Segurar: fechar seu portal"},
// ru
{"Проверяем, установлен ли на этом сервере Town Portal. Повторите попытку через несколько секунд.",
 "На этом сервере не установлен Town Portal, поэтому порталы здесь недоступны. Мод должен быть установлен и на сервере.",
 "Портал отменён.",
 "Переход через портал...",
 "Ваш портал закроется через %d с, чтобы другие успели вернуться.",
 "Отправиться: %s",
 "Вернуться: %s  /  Портал закроется",
 "Отправиться: %s (общий портал)",
 "Переход: %s...",
 "место открытия портала",
 "ваш Алтарь пламени",
 "Здесь нет места для портала: стена слишком близко.",
 "В пелене нельзя открывать порталы.",
 "%s: здесь нельзя открывать порталы.",
 "Ты всё спрашиваешь. Ответ тот же: портала не будет.",
 "Да, это кнопка портала. Ты всё жмёшь и жмёшь. Держи золотую звёздочку.",
 "Вот поэтому у тебя и нет пары, да?",
 "Загрузка… %d%%",
 "Портал закрыт.",
 "У вас нет открытого портала.",
 "Удерживать: закрыть портал"},
// zh-hans
{"正在检查此服务器是否运行 Town Portal。请几秒后再试。",
 "此服务器未运行 Town Portal，因此无法在此使用传送门。服务器也需要安装此模组。",
 "传送门已取消。",
 "正在穿过传送门...",
 "传送门将在%d秒后关闭，方便其他玩家跟随你返回。",
 "前往%s",
 "返回%s  /  传送门将关闭",
 "前往%s（共享传送门）",
 "正在前往%s...",
 "传送门开启处",
 "你的火焰祭坛",
 "这里没有空间开启传送门：墙壁太近了。",
 "无法在雾霭中开启传送门。",
 "%s：此处无法开启传送门。",
 "你一直问，答案还是一样：没有传送门。",
 "对，那就是传送门按钮。你一直在按。给你一朵小红花。",
 "这就是你单身的原因吧？",
 "加载中… %d%%",
 "传送门已关闭。",
 "你没有已开启的传送门。",
 "长按：关闭传送门"},
// zh-hant
{"正在檢查此伺服器是否執行 Town Portal。請幾秒後再試。",
 "此伺服器未執行 Town Portal，因此無法在此使用傳送門。伺服器也需要安裝此模組。",
 "傳送門已取消。",
 "正在穿過傳送門...",
 "傳送門將在%d秒後關閉，方便其他玩家跟隨你返回。",
 "前往%s",
 "返回%s  /  傳送門將關閉",
 "前往%s（共享傳送門）",
 "正在前往%s...",
 "傳送門開啟處",
 "你的火焰祭壇",
 "這裡沒有空間開啟傳送門：牆壁太近了。",
 "無法在霧靄中開啟傳送門。",
 "%s：此處無法開啟傳送門。",
 "你一直問，答案還是一樣：沒有傳送門。",
 "對，那就是傳送門按鈕。你一直在按。給你一朵小紅花。",
 "這就是你單身的原因吧？",
 "載入中… %d%%",
 "傳送門已關閉。",
 "你沒有已開啟的傳送門。",
 "長按：關閉傳送門"},
// tr
{"Bu sunucunun Town Portal kullanıp kullanmadığı hâlâ kontrol ediliyor. Birkaç saniye sonra tekrar dene.",
 "Bu sunucu Town Portal kullanmıyor, bu yüzden portallar burada kullanılamaz. Modun sunucuda da kurulu olması gerekir.",
 "Portal iptal edildi.",
 "Portaldan geçiliyor...",
 "Portalın %d saniye içinde kapanacak, böylece diğerleri seni takip edebilir.",
 "%s konumuna git",
 "%s konumuna dön  /  Portalın kapanır",
 "%s konumuna git (ortak portal)",
 "%s konumuna gidiliyor...",
 "portalın açıldığı yer",
 "Alev Sunağın",
 "Burada portal için yer yok: bir duvar çok yakın.",
 "Sis içinde portal açılamaz.",
 "%s: burada portal açılamaz.",
 "Sormaya devam ediyorsun. Cevap aynı: portal yok.",
 "Evet, portal tuşu bu. Basıp duruyorsun. Al bakalım, altın yıldızın.",
 "Bu yüzden sevgilin yok, değil mi?",
 "Yükleniyor… %%%d",
 "Portal kapatıldı.",
 "Açık bir portalın yok.",
 "Basılı tut: portalı kapat"},
// uk
{"Перевіряємо, чи встановлено на цьому сервері Town Portal. Спробуйте ще раз за кілька секунд.",
 "На цьому сервері не встановлено Town Portal, тому портали тут недоступні. Мод має бути встановлений і на сервері.",
 "Портал скасовано.",
 "Перехід через портал...",
 "Ваш портал закриється через %d с, щоб інші встигли повернутися.",
 "Вирушити: %s",
 "Повернутися: %s  /  Портал закриється",
 "Вирушити: %s (спільний портал)",
 "Перехід: %s...",
 "місце відкриття порталу",
 "ваш Вівтар полум'я",
 "Тут немає місця для порталу: стіна надто близько.",
 "У пелені не можна відкривати портали.",
 "%s: тут не можна відкривати портали.",
 "Ти все питаєш. Відповідь та сама: порталу не буде.",
 "Так, це кнопка порталу. Ти все тиснеш і тиснеш. Ось тобі золота зірочка.",
 "Ось чому в тебе немає пари, так?",
 "Завантаження… %d%%",
 "Портал закрито.",
 "У вас немає відкритого порталу.",
 "Утримувати: закрити портал"},
// th
{"กำลังตรวจสอบว่าเซิร์ฟเวอร์นี้ใช้ Town Portal หรือไม่ โปรดลองอีกครั้งในอีกไม่กี่วินาที",
 "เซิร์ฟเวอร์นี้ไม่ได้ใช้ Town Portal จึงใช้พอร์ทัลที่นี่ไม่ได้ เซิร์ฟเวอร์ต้องติดตั้งม็อดนี้ด้วย",
 "ยกเลิกพอร์ทัลแล้ว",
 "กำลังเดินทางผ่านพอร์ทัล...",
 "พอร์ทัลของคุณจะปิดใน %d วินาที เพื่อให้คนอื่นตามกลับมาได้",
 "เดินทางไป %s",
 "กลับไป %s  /  พอร์ทัลจะปิด",
 "เดินทางไป %s (พอร์ทัลที่ใช้ร่วมกัน)",
 "กำลังเดินทางไป %s...",
 "จุดที่เปิดพอร์ทัล",
 "แท่นบูชาเปลวไฟของคุณ",
 "ไม่มีพื้นที่สำหรับเปิดประตูมิติที่นี่ ผนังอยู่ใกล้เกินไป",
 "ไม่สามารถเปิดประตูมิติในม่านหมอกได้",
 "%s: ไม่สามารถเปิดประตูมิติที่นี่ได้",
 "ถามอีกกี่ครั้งคำตอบก็เหมือนเดิม ไม่มีประตูมิติ",
 "ใช่ นั่นแหละปุ่มประตูมิติ กดอยู่นั่นแหละ เอ้า รับดาวทองไปเลย",
 "นี่แหละเหตุผลที่ยังโสดอยู่ใช่ไหม",
 "กำลังโหลด… %d%%",
 "ปิดพอร์ทัลแล้ว",
 "คุณไม่มีพอร์ทัลที่เปิดอยู่",
 "กดค้าง: ปิดพอร์ทัล"},
};

inline const char* Get(Lang lang,Id id) {
    auto l=size_t(lang)<size_t(Lang::count)?size_t(lang):0;
    return kTexts[l][id];
}

// The game's language from its own word for "Legendary" (the rarity), as Find tells it.
// False when the text is empty or unknown; the caller keeps its previous language then.
inline bool Detect(std::string_view legendary,Lang& out) {
    auto has=[&](const char* part){return legendary.find(part)!=std::string_view::npos;};
    auto starts=[&](const char* prefix) {
        size_t n=strlen(prefix);if(legendary.size()<n)return false;
        for(size_t i=0;i<n;++i){char a=legendary[i];if(a>='A'&&a<='Z')a=char(a-'A'+'a');if(a!=prefix[i])return false;}
        return true;
    };
    if(legendary.empty())return false;
    if(starts("legendary"))out=Lang::en;
    else if(starts("legendär")||starts("legendÄr"))out=Lang::de;
    else if(starts("légend")||starts("lÉgend"))out=Lang::fr;
    else if(starts("legendari"))out=Lang::es;
    else if(starts("leggend"))out=Lang::it;
    else if(starts("lend"))out=Lang::pt;
    else if(starts("legendarn"))out=Lang::pl;
    else if(starts("efsane"))out=Lang::tr;
    else if(has("Леген")||has("ЛЕГЕН")||has("леген")) {
        // Russian or Ukrainian: the letters only Ukrainian has, else its endings (-ний, -на, -не);
        // the rest is Russian, whichever form the game uses (-ный, -ая, -ое, ...).
        auto ends=[&](const char* suffix){size_t n=strlen(suffix);return legendary.size()>=n&&legendary.substr(legendary.size()-n)==suffix;};
        bool uk=has("і")||has("І")||has("ї")||has("Ї")||has("є")||has("Є")||has("ґ")||has("Ґ")||
                ends("ний")||ends("на")||ends("не")||ends("НИЙ")||ends("НА")||ends("НЕ");
        out=uk&&!has("ы")&&!has("Ы")?Lang::uk:Lang::ru;
    }
    else if(has("전설"))out=Lang::ko;
    else if(has("レジェ")||has("伝説"))out=Lang::ja;
    else if(has("传")||has("说"))out=Lang::zhHans;
    else if(has("傳")||has("說"))out=Lang::zhHant;
    else if(legendary.size()>=3&&uint8_t(legendary[0])==0xE0&&(uint8_t(legendary[1])==0xB8||uint8_t(legendary[1])==0xB9))out=Lang::th;
    else return false;
    return true;
}

// UTF-8: the code point at s[i], and its length in bytes.
inline uint32_t Decode(std::string_view s,size_t i,size_t& n) {
    auto b=uint8_t(s[i]);
    n=b<0x80?1:b>=0xF0?4:b>=0xE0?3:b>=0xC0?2:1;
    if(i+n>s.size()){n=1;return b;}
    uint32_t c=n==1?b:n==2?b&0x1f:n==3?b&0x0f:b&0x07;
    for(size_t k=1;k<n;++k)c=(c<<6)|(uint8_t(s[i+k])&0x3f);
    return c;
}
// Width in Latin letters: CJK and Hangul are about two, Thai marks above or below a letter none.
inline unsigned Columns(uint32_t c) {
    if(c==0x0E31||(c>=0x0E34&&c<=0x0E3A)||(c>=0x0E47&&c<=0x0E4E))return 0;
    if((c>=0x1100&&c<=0x115F)||(c>=0x2E80&&c<=0xA4CF)||(c>=0xAC00&&c<=0xD7A3)||(c>=0xF900&&c<=0xFAFF)||
       (c>=0xFE30&&c<=0xFE4F)||(c>=0xFF00&&c<=0xFF60)||(c>=0xFFE0&&c<=0xFFE6))return 2;
    return 1;
}
// Punctuation that must not start a line in Chinese and Japanese.
inline bool Closing(uint32_t c) {
    for(uint32_t p:{0x3001u,0x3002u,0xFF0Cu,0xFF0Eu,0xFF01u,0xFF1Fu,0xFF1Au,0xFF1Bu,0xFF09u,0x300Du,0x300Fu,0x3011u,0x30FCu,0x2026u})
        if(c==p)return true;
    return c=='.'||c==','||c=='!'||c=='?'||c==':'||c==';'||c==')';
}
// Lines of at most width columns. Breaks at spaces, between CJK characters, and, with no
// other choice (Thai writes words without spaces), between letters.
inline std::vector<std::string> Wrap(std::string_view text,size_t width) {
    std::vector<std::string> lines;
    if(!width)return lines;
    while(!text.empty()) {
        while(!text.empty()&&text.front()==' ')text.remove_prefix(1);
        if(text.empty())break;
        size_t take=text.size(),candidate=0,columns=0,n=0;bool wideBefore=false;
        for(size_t i=0;i<text.size();i+=n) {
            auto c=Decode(text,i,n);auto w=Columns(c);
            if(i&&c==' ')candidate=i;
            else if(i&&w&&(w==2||wideBefore)&&!Closing(c))candidate=i;
            if(i&&columns+w>width){take=candidate?candidate:i;break;}
            columns+=w;if(w)wideBefore=w==2;
        }
        lines.emplace_back(text.substr(0,take));text.remove_prefix(take);
    }
    return lines;
}

} // namespace portal::text
