//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <xrpl/beast/utility/instrumentation.h>
#include <xrpl/crypto/RFC1751.h>
#include <boost/algorithm/string.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <ranges>
#include <span>
#include <string>
#include <vector>

namespace ripple {
namespace rfc1751 {

namespace {

/** The list of words we use.

    The list may appear sorted, but it is not: it is composed of two
    separately sorted parts: the short words, which are shorter than
    four characters long, come first; the remaining words, which are
    all exactly four characters long, follow.
 */
constexpr std::string_view wordlist[] = {
    "A",    "ABE",  "ACE",  "ACT",  "AD",   "ADA",  "ADD",  "AGO",  "AID",
    "AIM",  "AIR",  "ALL",  "ALP",  "AM",   "AMY",  "AN",   "ANA",  "AND",
    "ANN",  "ANT",  "ANY",  "APE",  "APS",  "APT",  "ARC",  "ARE",  "ARK",
    "ARM",  "ART",  "AS",   "ASH",  "ASK",  "AT",   "ATE",  "AUG",  "AUK",
    "AVE",  "AWE",  "AWK",  "AWL",  "AWN",  "AX",   "AYE",  "BAD",  "BAG",
    "BAH",  "BAM",  "BAN",  "BAR",  "BAT",  "BAY",  "BE",   "BED",  "BEE",
    "BEG",  "BEN",  "BET",  "BEY",  "BIB",  "BID",  "BIG",  "BIN",  "BIT",
    "BOB",  "BOG",  "BON",  "BOO",  "BOP",  "BOW",  "BOY",  "BUB",  "BUD",
    "BUG",  "BUM",  "BUN",  "BUS",  "BUT",  "BUY",  "BY",   "BYE",  "CAB",
    "CAL",  "CAM",  "CAN",  "CAP",  "CAR",  "CAT",  "CAW",  "COD",  "COG",
    "COL",  "CON",  "COO",  "COP",  "COT",  "COW",  "COY",  "CRY",  "CUB",
    "CUE",  "CUP",  "CUR",  "CUT",  "DAB",  "DAD",  "DAM",  "DAN",  "DAR",
    "DAY",  "DEE",  "DEL",  "DEN",  "DES",  "DEW",  "DID",  "DIE",  "DIG",
    "DIN",  "DIP",  "DO",   "DOE",  "DOG",  "DON",  "DOT",  "DOW",  "DRY",
    "DUB",  "DUD",  "DUE",  "DUG",  "DUN",  "EAR",  "EAT",  "ED",   "EEL",
    "EGG",  "EGO",  "ELI",  "ELK",  "ELM",  "ELY",  "EM",   "END",  "EST",
    "ETC",  "EVA",  "EVE",  "EWE",  "EYE",  "FAD",  "FAN",  "FAR",  "FAT",
    "FAY",  "FED",  "FEE",  "FEW",  "FIB",  "FIG",  "FIN",  "FIR",  "FIT",
    "FLO",  "FLY",  "FOE",  "FOG",  "FOR",  "FRY",  "FUM",  "FUN",  "FUR",
    "GAB",  "GAD",  "GAG",  "GAL",  "GAM",  "GAP",  "GAS",  "GAY",  "GEE",
    "GEL",  "GEM",  "GET",  "GIG",  "GIL",  "GIN",  "GO",   "GOT",  "GUM",
    "GUN",  "GUS",  "GUT",  "GUY",  "GYM",  "GYP",  "HA",   "HAD",  "HAL",
    "HAM",  "HAN",  "HAP",  "HAS",  "HAT",  "HAW",  "HAY",  "HE",   "HEM",
    "HEN",  "HER",  "HEW",  "HEY",  "HI",   "HID",  "HIM",  "HIP",  "HIS",
    "HIT",  "HO",   "HOB",  "HOC",  "HOE",  "HOG",  "HOP",  "HOT",  "HOW",
    "HUB",  "HUE",  "HUG",  "HUH",  "HUM",  "HUT",  "I",    "ICY",  "IDA",
    "IF",   "IKE",  "ILL",  "INK",  "INN",  "IO",   "ION",  "IQ",   "IRA",
    "IRE",  "IRK",  "IS",   "IT",   "ITS",  "IVY",  "JAB",  "JAG",  "JAM",
    "JAN",  "JAR",  "JAW",  "JAY",  "JET",  "JIG",  "JIM",  "JO",   "JOB",
    "JOE",  "JOG",  "JOT",  "JOY",  "JUG",  "JUT",  "KAY",  "KEG",  "KEN",
    "KEY",  "KID",  "KIM",  "KIN",  "KIT",  "LA",   "LAB",  "LAC",  "LAD",
    "LAG",  "LAM",  "LAP",  "LAW",  "LAY",  "LEA",  "LED",  "LEE",  "LEG",
    "LEN",  "LEO",  "LET",  "LEW",  "LID",  "LIE",  "LIN",  "LIP",  "LIT",
    "LO",   "LOB",  "LOG",  "LOP",  "LOS",  "LOT",  "LOU",  "LOW",  "LOY",
    "LUG",  "LYE",  "MA",   "MAC",  "MAD",  "MAE",  "MAN",  "MAO",  "MAP",
    "MAT",  "MAW",  "MAY",  "ME",   "MEG",  "MEL",  "MEN",  "MET",  "MEW",
    "MID",  "MIN",  "MIT",  "MOB",  "MOD",  "MOE",  "MOO",  "MOP",  "MOS",
    "MOT",  "MOW",  "MUD",  "MUG",  "MUM",  "MY",   "NAB",  "NAG",  "NAN",
    "NAP",  "NAT",  "NAY",  "NE",   "NED",  "NEE",  "NET",  "NEW",  "NIB",
    "NIL",  "NIP",  "NIT",  "NO",   "NOB",  "NOD",  "NON",  "NOR",  "NOT",
    "NOV",  "NOW",  "NU",   "NUN",  "NUT",  "O",    "OAF",  "OAK",  "OAR",
    "OAT",  "ODD",  "ODE",  "OF",   "OFF",  "OFT",  "OH",   "OIL",  "OK",
    "OLD",  "ON",   "ONE",  "OR",   "ORB",  "ORE",  "ORR",  "OS",   "OTT",
    "OUR",  "OUT",  "OVA",  "OW",   "OWE",  "OWL",  "OWN",  "OX",   "PA",
    "PAD",  "PAL",  "PAM",  "PAN",  "PAP",  "PAR",  "PAT",  "PAW",  "PAY",
    "PEA",  "PEG",  "PEN",  "PEP",  "PER",  "PET",  "PEW",  "PHI",  "PI",
    "PIE",  "PIN",  "PIT",  "PLY",  "PO",   "POD",  "POE",  "POP",  "POT",
    "POW",  "PRO",  "PRY",  "PUB",  "PUG",  "PUN",  "PUP",  "PUT",  "QUO",
    "RAG",  "RAM",  "RAN",  "RAP",  "RAT",  "RAW",  "RAY",  "REB",  "RED",
    "REP",  "RET",  "RIB",  "RID",  "RIG",  "RIM",  "RIO",  "RIP",  "ROB",
    "ROD",  "ROE",  "RON",  "ROT",  "ROW",  "ROY",  "RUB",  "RUE",  "RUG",
    "RUM",  "RUN",  "RYE",  "SAC",  "SAD",  "SAG",  "SAL",  "SAM",  "SAN",
    "SAP",  "SAT",  "SAW",  "SAY",  "SEA",  "SEC",  "SEE",  "SEN",  "SET",
    "SEW",  "SHE",  "SHY",  "SIN",  "SIP",  "SIR",  "SIS",  "SIT",  "SKI",
    "SKY",  "SLY",  "SO",   "SOB",  "SOD",  "SON",  "SOP",  "SOW",  "SOY",
    "SPA",  "SPY",  "SUB",  "SUD",  "SUE",  "SUM",  "SUN",  "SUP",  "TAB",
    "TAD",  "TAG",  "TAN",  "TAP",  "TAR",  "TEA",  "TED",  "TEE",  "TEN",
    "THE",  "THY",  "TIC",  "TIE",  "TIM",  "TIN",  "TIP",  "TO",   "TOE",
    "TOG",  "TOM",  "TON",  "TOO",  "TOP",  "TOW",  "TOY",  "TRY",  "TUB",
    "TUG",  "TUM",  "TUN",  "TWO",  "UN",   "UP",   "US",   "USE",  "VAN",
    "VAT",  "VET",  "VIE",  "WAD",  "WAG",  "WAR",  "WAS",  "WAY",  "WE",
    "WEB",  "WED",  "WEE",  "WET",  "WHO",  "WHY",  "WIN",  "WIT",  "WOK",
    "WON",  "WOO",  "WOW",  "WRY",  "WU",   "YAM",  "YAP",  "YAW",  "YE",
    "YEA",  "YES",  "YET",  "YOU",  "ABED", "ABEL", "ABET", "ABLE", "ABUT",
    "ACHE", "ACID", "ACME", "ACRE", "ACTA", "ACTS", "ADAM", "ADDS", "ADEN",
    "AFAR", "AFRO", "AGEE", "AHEM", "AHOY", "AIDA", "AIDE", "AIDS", "AIRY",
    "AJAR", "AKIN", "ALAN", "ALEC", "ALGA", "ALIA", "ALLY", "ALMA", "ALOE",
    "ALSO", "ALTO", "ALUM", "ALVA", "AMEN", "AMES", "AMID", "AMMO", "AMOK",
    "AMOS", "AMRA", "ANDY", "ANEW", "ANNA", "ANNE", "ANTE", "ANTI", "AQUA",
    "ARAB", "ARCH", "AREA", "ARGO", "ARID", "ARMY", "ARTS", "ARTY", "ASIA",
    "ASKS", "ATOM", "AUNT", "AURA", "AUTO", "AVER", "AVID", "AVIS", "AVON",
    "AVOW", "AWAY", "AWRY", "BABE", "BABY", "BACH", "BACK", "BADE", "BAIL",
    "BAIT", "BAKE", "BALD", "BALE", "BALI", "BALK", "BALL", "BALM", "BAND",
    "BANE", "BANG", "BANK", "BARB", "BARD", "BARE", "BARK", "BARN", "BARR",
    "BASE", "BASH", "BASK", "BASS", "BATE", "BATH", "BAWD", "BAWL", "BEAD",
    "BEAK", "BEAM", "BEAN", "BEAR", "BEAT", "BEAU", "BECK", "BEEF", "BEEN",
    "BEER", "BEET", "BELA", "BELL", "BELT", "BEND", "BENT", "BERG", "BERN",
    "BERT", "BESS", "BEST", "BETA", "BETH", "BHOY", "BIAS", "BIDE", "BIEN",
    "BILE", "BILK", "BILL", "BIND", "BING", "BIRD", "BITE", "BITS", "BLAB",
    "BLAT", "BLED", "BLEW", "BLOB", "BLOC", "BLOT", "BLOW", "BLUE", "BLUM",
    "BLUR", "BOAR", "BOAT", "BOCA", "BOCK", "BODE", "BODY", "BOGY", "BOHR",
    "BOIL", "BOLD", "BOLO", "BOLT", "BOMB", "BONA", "BOND", "BONE", "BONG",
    "BONN", "BONY", "BOOK", "BOOM", "BOON", "BOOT", "BORE", "BORG", "BORN",
    "BOSE", "BOSS", "BOTH", "BOUT", "BOWL", "BOYD", "BRAD", "BRAE", "BRAG",
    "BRAN", "BRAY", "BRED", "BREW", "BRIG", "BRIM", "BROW", "BUCK", "BUDD",
    "BUFF", "BULB", "BULK", "BULL", "BUNK", "BUNT", "BUOY", "BURG", "BURL",
    "BURN", "BURR", "BURT", "BURY", "BUSH", "BUSS", "BUST", "BUSY", "BYTE",
    "CADY", "CAFE", "CAGE", "CAIN", "CAKE", "CALF", "CALL", "CALM", "CAME",
    "CANE", "CANT", "CARD", "CARE", "CARL", "CARR", "CART", "CASE", "CASH",
    "CASK", "CAST", "CAVE", "CEIL", "CELL", "CENT", "CERN", "CHAD", "CHAR",
    "CHAT", "CHAW", "CHEF", "CHEN", "CHEW", "CHIC", "CHIN", "CHOU", "CHOW",
    "CHUB", "CHUG", "CHUM", "CITE", "CITY", "CLAD", "CLAM", "CLAN", "CLAW",
    "CLAY", "CLOD", "CLOG", "CLOT", "CLUB", "CLUE", "COAL", "COAT", "COCA",
    "COCK", "COCO", "CODA", "CODE", "CODY", "COED", "COIL", "COIN", "COKE",
    "COLA", "COLD", "COLT", "COMA", "COMB", "COME", "COOK", "COOL", "COON",
    "COOT", "CORD", "CORE", "CORK", "CORN", "COST", "COVE", "COWL", "CRAB",
    "CRAG", "CRAM", "CRAY", "CREW", "CRIB", "CROW", "CRUD", "CUBA", "CUBE",
    "CUFF", "CULL", "CULT", "CUNY", "CURB", "CURD", "CURE", "CURL", "CURT",
    "CUTS", "DADE", "DALE", "DAME", "DANA", "DANE", "DANG", "DANK", "DARE",
    "DARK", "DARN", "DART", "DASH", "DATA", "DATE", "DAVE", "DAVY", "DAWN",
    "DAYS", "DEAD", "DEAF", "DEAL", "DEAN", "DEAR", "DEBT", "DECK", "DEED",
    "DEEM", "DEER", "DEFT", "DEFY", "DELL", "DENT", "DENY", "DESK", "DIAL",
    "DICE", "DIED", "DIET", "DIME", "DINE", "DING", "DINT", "DIRE", "DIRT",
    "DISC", "DISH", "DISK", "DIVE", "DOCK", "DOES", "DOLE", "DOLL", "DOLT",
    "DOME", "DONE", "DOOM", "DOOR", "DORA", "DOSE", "DOTE", "DOUG", "DOUR",
    "DOVE", "DOWN", "DRAB", "DRAG", "DRAM", "DRAW", "DREW", "DRUB", "DRUG",
    "DRUM", "DUAL", "DUCK", "DUCT", "DUEL", "DUET", "DUKE", "DULL", "DUMB",
    "DUNE", "DUNK", "DUSK", "DUST", "DUTY", "EACH", "EARL", "EARN", "EASE",
    "EAST", "EASY", "EBEN", "ECHO", "EDDY", "EDEN", "EDGE", "EDGY", "EDIT",
    "EDNA", "EGAN", "ELAN", "ELBA", "ELLA", "ELSE", "EMIL", "EMIT", "EMMA",
    "ENDS", "ERIC", "EROS", "EVEN", "EVER", "EVIL", "EYED", "FACE", "FACT",
    "FADE", "FAIL", "FAIN", "FAIR", "FAKE", "FALL", "FAME", "FANG", "FARM",
    "FAST", "FATE", "FAWN", "FEAR", "FEAT", "FEED", "FEEL", "FEET", "FELL",
    "FELT", "FEND", "FERN", "FEST", "FEUD", "FIEF", "FIGS", "FILE", "FILL",
    "FILM", "FIND", "FINE", "FINK", "FIRE", "FIRM", "FISH", "FISK", "FIST",
    "FITS", "FIVE", "FLAG", "FLAK", "FLAM", "FLAT", "FLAW", "FLEA", "FLED",
    "FLEW", "FLIT", "FLOC", "FLOG", "FLOW", "FLUB", "FLUE", "FOAL", "FOAM",
    "FOGY", "FOIL", "FOLD", "FOLK", "FOND", "FONT", "FOOD", "FOOL", "FOOT",
    "FORD", "FORE", "FORK", "FORM", "FORT", "FOSS", "FOUL", "FOUR", "FOWL",
    "FRAU", "FRAY", "FRED", "FREE", "FRET", "FREY", "FROG", "FROM", "FUEL",
    "FULL", "FUME", "FUND", "FUNK", "FURY", "FUSE", "FUSS", "GAFF", "GAGE",
    "GAIL", "GAIN", "GAIT", "GALA", "GALE", "GALL", "GALT", "GAME", "GANG",
    "GARB", "GARY", "GASH", "GATE", "GAUL", "GAUR", "GAVE", "GAWK", "GEAR",
    "GELD", "GENE", "GENT", "GERM", "GETS", "GIBE", "GIFT", "GILD", "GILL",
    "GILT", "GINA", "GIRD", "GIRL", "GIST", "GIVE", "GLAD", "GLEE", "GLEN",
    "GLIB", "GLOB", "GLOM", "GLOW", "GLUE", "GLUM", "GLUT", "GOAD", "GOAL",
    "GOAT", "GOER", "GOES", "GOLD", "GOLF", "GONE", "GONG", "GOOD", "GOOF",
    "GORE", "GORY", "GOSH", "GOUT", "GOWN", "GRAB", "GRAD", "GRAY", "GREG",
    "GREW", "GREY", "GRID", "GRIM", "GRIN", "GRIT", "GROW", "GRUB", "GULF",
    "GULL", "GUNK", "GURU", "GUSH", "GUST", "GWEN", "GWYN", "HAAG", "HAAS",
    "HACK", "HAIL", "HAIR", "HALE", "HALF", "HALL", "HALO", "HALT", "HAND",
    "HANG", "HANK", "HANS", "HARD", "HARK", "HARM", "HART", "HASH", "HAST",
    "HATE", "HATH", "HAUL", "HAVE", "HAWK", "HAYS", "HEAD", "HEAL", "HEAR",
    "HEAT", "HEBE", "HECK", "HEED", "HEEL", "HEFT", "HELD", "HELL", "HELM",
    "HERB", "HERD", "HERE", "HERO", "HERS", "HESS", "HEWN", "HICK", "HIDE",
    "HIGH", "HIKE", "HILL", "HILT", "HIND", "HINT", "HIRE", "HISS", "HIVE",
    "HOBO", "HOCK", "HOFF", "HOLD", "HOLE", "HOLM", "HOLT", "HOME", "HONE",
    "HONK", "HOOD", "HOOF", "HOOK", "HOOT", "HORN", "HOSE", "HOST", "HOUR",
    "HOVE", "HOWE", "HOWL", "HOYT", "HUCK", "HUED", "HUFF", "HUGE", "HUGH",
    "HUGO", "HULK", "HULL", "HUNK", "HUNT", "HURD", "HURL", "HURT", "HUSH",
    "HYDE", "HYMN", "IBIS", "ICON", "IDEA", "IDLE", "IFFY", "INCA", "INCH",
    "INTO", "IONS", "IOTA", "IOWA", "IRIS", "IRMA", "IRON", "ISLE", "ITCH",
    "ITEM", "IVAN", "JACK", "JADE", "JAIL", "JAKE", "JANE", "JAVA", "JEAN",
    "JEFF", "JERK", "JESS", "JEST", "JIBE", "JILL", "JILT", "JIVE", "JOAN",
    "JOBS", "JOCK", "JOEL", "JOEY", "JOHN", "JOIN", "JOKE", "JOLT", "JOVE",
    "JUDD", "JUDE", "JUDO", "JUDY", "JUJU", "JUKE", "JULY", "JUNE", "JUNK",
    "JUNO", "JURY", "JUST", "JUTE", "KAHN", "KALE", "KANE", "KANT", "KARL",
    "KATE", "KEEL", "KEEN", "KENO", "KENT", "KERN", "KERR", "KEYS", "KICK",
    "KILL", "KIND", "KING", "KIRK", "KISS", "KITE", "KLAN", "KNEE", "KNEW",
    "KNIT", "KNOB", "KNOT", "KNOW", "KOCH", "KONG", "KUDO", "KURD", "KURT",
    "KYLE", "LACE", "LACK", "LACY", "LADY", "LAID", "LAIN", "LAIR", "LAKE",
    "LAMB", "LAME", "LAND", "LANE", "LANG", "LARD", "LARK", "LASS", "LAST",
    "LATE", "LAUD", "LAVA", "LAWN", "LAWS", "LAYS", "LEAD", "LEAF", "LEAK",
    "LEAN", "LEAR", "LEEK", "LEER", "LEFT", "LEND", "LENS", "LENT", "LEON",
    "LESK", "LESS", "LEST", "LETS", "LIAR", "LICE", "LICK", "LIED", "LIEN",
    "LIES", "LIEU", "LIFE", "LIFT", "LIKE", "LILA", "LILT", "LILY", "LIMA",
    "LIMB", "LIME", "LIND", "LINE", "LINK", "LINT", "LION", "LISA", "LIST",
    "LIVE", "LOAD", "LOAF", "LOAM", "LOAN", "LOCK", "LOFT", "LOGE", "LOIS",
    "LOLA", "LONE", "LONG", "LOOK", "LOON", "LOOT", "LORD", "LORE", "LOSE",
    "LOSS", "LOST", "LOUD", "LOVE", "LOWE", "LUCK", "LUCY", "LUGE", "LUKE",
    "LULU", "LUND", "LUNG", "LURA", "LURE", "LURK", "LUSH", "LUST", "LYLE",
    "LYNN", "LYON", "LYRA", "MACE", "MADE", "MAGI", "MAID", "MAIL", "MAIN",
    "MAKE", "MALE", "MALI", "MALL", "MALT", "MANA", "MANN", "MANY", "MARC",
    "MARE", "MARK", "MARS", "MART", "MARY", "MASH", "MASK", "MASS", "MAST",
    "MATE", "MATH", "MAUL", "MAYO", "MEAD", "MEAL", "MEAN", "MEAT", "MEEK",
    "MEET", "MELD", "MELT", "MEMO", "MEND", "MENU", "MERT", "MESH", "MESS",
    "MICE", "MIKE", "MILD", "MILE", "MILK", "MILL", "MILT", "MIMI", "MIND",
    "MINE", "MINI", "MINK", "MINT", "MIRE", "MISS", "MIST", "MITE", "MITT",
    "MOAN", "MOAT", "MOCK", "MODE", "MOLD", "MOLE", "MOLL", "MOLT", "MONA",
    "MONK", "MONT", "MOOD", "MOON", "MOOR", "MOOT", "MORE", "MORN", "MORT",
    "MOSS", "MOST", "MOTH", "MOVE", "MUCH", "MUCK", "MUDD", "MUFF", "MULE",
    "MULL", "MURK", "MUSH", "MUST", "MUTE", "MUTT", "MYRA", "MYTH", "NAGY",
    "NAIL", "NAIR", "NAME", "NARY", "NASH", "NAVE", "NAVY", "NEAL", "NEAR",
    "NEAT", "NECK", "NEED", "NEIL", "NELL", "NEON", "NERO", "NESS", "NEST",
    "NEWS", "NEWT", "NIBS", "NICE", "NICK", "NILE", "NINA", "NINE", "NOAH",
    "NODE", "NOEL", "NOLL", "NONE", "NOOK", "NOON", "NORM", "NOSE", "NOTE",
    "NOUN", "NOVA", "NUDE", "NULL", "NUMB", "OATH", "OBEY", "OBOE", "ODIN",
    "OHIO", "OILY", "OINT", "OKAY", "OLAF", "OLDY", "OLGA", "OLIN", "OMAN",
    "OMEN", "OMIT", "ONCE", "ONES", "ONLY", "ONTO", "ONUS", "ORAL", "ORGY",
    "OSLO", "OTIS", "OTTO", "OUCH", "OUST", "OUTS", "OVAL", "OVEN", "OVER",
    "OWLY", "OWNS", "QUAD", "QUIT", "QUOD", "RACE", "RACK", "RACY", "RAFT",
    "RAGE", "RAID", "RAIL", "RAIN", "RAKE", "RANK", "RANT", "RARE", "RASH",
    "RATE", "RAVE", "RAYS", "READ", "REAL", "REAM", "REAR", "RECK", "REED",
    "REEF", "REEK", "REEL", "REID", "REIN", "RENA", "REND", "RENT", "REST",
    "RICE", "RICH", "RICK", "RIDE", "RIFT", "RILL", "RIME", "RING", "RINK",
    "RISE", "RISK", "RITE", "ROAD", "ROAM", "ROAR", "ROBE", "ROCK", "RODE",
    "ROIL", "ROLL", "ROME", "ROOD", "ROOF", "ROOK", "ROOM", "ROOT", "ROSA",
    "ROSE", "ROSS", "ROSY", "ROTH", "ROUT", "ROVE", "ROWE", "ROWS", "RUBE",
    "RUBY", "RUDE", "RUDY", "RUIN", "RULE", "RUNG", "RUNS", "RUNT", "RUSE",
    "RUSH", "RUSK", "RUSS", "RUST", "RUTH", "SACK", "SAFE", "SAGE", "SAID",
    "SAIL", "SALE", "SALK", "SALT", "SAME", "SAND", "SANE", "SANG", "SANK",
    "SARA", "SAUL", "SAVE", "SAYS", "SCAN", "SCAR", "SCAT", "SCOT", "SEAL",
    "SEAM", "SEAR", "SEAT", "SEED", "SEEK", "SEEM", "SEEN", "SEES", "SELF",
    "SELL", "SEND", "SENT", "SETS", "SEWN", "SHAG", "SHAM", "SHAW", "SHAY",
    "SHED", "SHIM", "SHIN", "SHOD", "SHOE", "SHOT", "SHOW", "SHUN", "SHUT",
    "SICK", "SIDE", "SIFT", "SIGH", "SIGN", "SILK", "SILL", "SILO", "SILT",
    "SINE", "SING", "SINK", "SIRE", "SITE", "SITS", "SITU", "SKAT", "SKEW",
    "SKID", "SKIM", "SKIN", "SKIT", "SLAB", "SLAM", "SLAT", "SLAY", "SLED",
    "SLEW", "SLID", "SLIM", "SLIT", "SLOB", "SLOG", "SLOT", "SLOW", "SLUG",
    "SLUM", "SLUR", "SMOG", "SMUG", "SNAG", "SNOB", "SNOW", "SNUB", "SNUG",
    "SOAK", "SOAR", "SOCK", "SODA", "SOFA", "SOFT", "SOIL", "SOLD", "SOME",
    "SONG", "SOON", "SOOT", "SORE", "SORT", "SOUL", "SOUR", "SOWN", "STAB",
    "STAG", "STAN", "STAR", "STAY", "STEM", "STEW", "STIR", "STOW", "STUB",
    "STUN", "SUCH", "SUDS", "SUIT", "SULK", "SUMS", "SUNG", "SUNK", "SURE",
    "SURF", "SWAB", "SWAG", "SWAM", "SWAN", "SWAT", "SWAY", "SWIM", "SWUM",
    "TACK", "TACT", "TAIL", "TAKE", "TALE", "TALK", "TALL", "TANK", "TASK",
    "TATE", "TAUT", "TEAL", "TEAM", "TEAR", "TECH", "TEEM", "TEEN", "TEET",
    "TELL", "TEND", "TENT", "TERM", "TERN", "TESS", "TEST", "THAN", "THAT",
    "THEE", "THEM", "THEN", "THEY", "THIN", "THIS", "THUD", "THUG", "TICK",
    "TIDE", "TIDY", "TIED", "TIER", "TILE", "TILL", "TILT", "TIME", "TINA",
    "TINE", "TINT", "TINY", "TIRE", "TOAD", "TOGO", "TOIL", "TOLD", "TOLL",
    "TONE", "TONG", "TONY", "TOOK", "TOOL", "TOOT", "TORE", "TORN", "TOTE",
    "TOUR", "TOUT", "TOWN", "TRAG", "TRAM", "TRAY", "TREE", "TREK", "TRIG",
    "TRIM", "TRIO", "TROD", "TROT", "TROY", "TRUE", "TUBA", "TUBE", "TUCK",
    "TUFT", "TUNA", "TUNE", "TUNG", "TURF", "TURN", "TUSK", "TWIG", "TWIN",
    "TWIT", "ULAN", "UNIT", "URGE", "USED", "USER", "USES", "UTAH", "VAIL",
    "VAIN", "VALE", "VARY", "VASE", "VAST", "VEAL", "VEDA", "VEIL", "VEIN",
    "VEND", "VENT", "VERB", "VERY", "VETO", "VICE", "VIEW", "VINE", "VISE",
    "VOID", "VOLT", "VOTE", "WACK", "WADE", "WAGE", "WAIL", "WAIT", "WAKE",
    "WALE", "WALK", "WALL", "WALT", "WAND", "WANE", "WANG", "WANT", "WARD",
    "WARM", "WARN", "WART", "WASH", "WAST", "WATS", "WATT", "WAVE", "WAVY",
    "WAYS", "WEAK", "WEAL", "WEAN", "WEAR", "WEED", "WEEK", "WEIR", "WELD",
    "WELL", "WELT", "WENT", "WERE", "WERT", "WEST", "WHAM", "WHAT", "WHEE",
    "WHEN", "WHET", "WHOA", "WHOM", "WICK", "WIFE", "WILD", "WILL", "WIND",
    "WINE", "WING", "WINK", "WINO", "WIRE", "WISE", "WISH", "WITH", "WOLF",
    "WONT", "WOOD", "WOOL", "WORD", "WORE", "WORK", "WORM", "WORN", "WOVE",
    "WRIT", "WYNN", "YALE", "YANG", "YANK", "YARD", "YARN", "YAWL", "YAWN",
    "YEAH", "YEAR", "YELL", "YOGA", "YOKE"};

static_assert(
    []() consteval {
        if (!std::has_single_bit(std::size(wordlist)))
            return false;

        auto check = [](std::string_view curr,
                        std::string_view prev) consteval {
            if (prev.size() == 4 && curr.size() < 4)
                return false;

            // At the partition point (the boundary between short and long
            // words) the lexicographical ordering breaks.
            if (prev.size() < 4 && curr.size() == 4)
                return true;

            return curr > prev;
        };

        std::string_view last;

        for (auto word : wordlist)
        {
            if (word.empty() || word.size() > 4)
                return false;

            if (!last.empty() && !check(word, last))
                return false;

            last = word;
        }

        return true;
    }(),
    "rfc1751 wordlist incorrectly sized or improperly sorted");

/** The cutoff point between "short" and "long" words in the wordlist */
constexpr std::size_t dictionaryPartition = []() consteval {
    for (std::size_t i = 0; i < std::size(wordlist); ++i)
    {
        if (wordlist[i].size() == 4)
            return i;
    }
    throw "no partition boundary found";
}();

/** How many bits each word encodes. Depends on the size of the table. */
constexpr auto bitsPerWord = std::countr_zero(std::size(wordlist));

// Extract `length` bits from byte array `s` starting at bit `start`.
[[nodiscard]] constexpr std::size_t
extractBits(
    std::span<std::uint8_t const, 9> s,
    std::size_t start,
    std::size_t length) noexcept
{
    auto const byte = start / 8;
    auto const shift = 24 - (length + (start % 8));
    auto const mask = (std::size_t{1} << length) - 1;

    // Load up to 3 bytes straddling the target bits into a 24-bit window.
    std::size_t window = s[byte] << 16;

    if (shift < 16)
        window |= s[byte + 1] << 8;

    if (shift < 8)
        window |= s[byte + 2];

    return (window >> shift) & mask;
}

// Insert `length` bits of `x` into byte array `s` at bit position `start`.
constexpr std::size_t
insertBits(
    std::span<std::uint8_t, 9> s,
    std::size_t x,
    std::size_t start) noexcept
{
    auto const byte = start / 8;
    auto const y = x << (24 - (start % 8) - bitsPerWord);

    s[byte] |= static_cast<std::uint8_t>((y >> 16) & 0xff);
    s[byte + 1] |= static_cast<std::uint8_t>((y >> 8) & 0xff);
    s[byte + 2] |= static_cast<std::uint8_t>(y & 0xff);

    return bitsPerWord;
}

[[nodiscard]] constexpr std::size_t
parityBits(std::span<std::uint8_t const, 9> buf) noexcept
{
    std::size_t parity = 0;

    for (std::size_t i = 0; i < 64; i += 2)
        parity += extractBits(buf, i, 2);

    return parity & 3;
}

// Normalize a word to uppercase with common substitutions (1->L, 0->O, 5->S).
[[nodiscard]] constexpr std::string
normalize(std::string word)
{
    for (auto& ch : word)
    {
        if (ch >= 'a' && ch <= 'z')
            ch -= ('a' - 'A');
        else if (ch == '1')
            ch = 'L';
        else if (ch == '0')
            ch = 'O';
        else if (ch == '5')
            ch = 'S';
    }

    return word;
}

// Binary search the wordlist for `word`.
[[nodiscard]] constexpr std::optional<std::size_t>
dictionaryLookup(std::string_view const& word) noexcept
{
    auto const begin = (word.size() < 4)
        ? std::begin(wordlist)
        : std::begin(wordlist) + dictionaryPartition;

    auto const end = (word.size() < 4)
        ? std::begin(wordlist) + dictionaryPartition
        : std::end(wordlist);

    auto it = std::lower_bound(begin, end, word);

    if (it != end && *it == word)
        return static_cast<std::size_t>(
            std::distance(std::begin(wordlist), it));

    return std::nullopt;
}

// Encode 8 bytes as 6 words.
[[nodiscard]] constexpr std::array<std::string_view, 6>
bytesToEnglish(std::span<std::uint8_t const, 8> data)
{
    std::array<std::uint8_t, 9> buf{};
    std::copy_n(data.data(), data.size(), buf.data());

    buf[8] = static_cast<std::uint8_t>(parityBits(buf) << 6);

    std::array<std::string_view, 6> words;

    for (std::size_t i = 0; i < 6; ++i)
        words[i] = wordlist[extractBits(buf, i * bitsPerWord, bitsPerWord)];

    return words;
}

// Decode 6 words into 8 bytes. Returns true on success.
[[nodiscard]] constexpr bool
englishToBytes(
    std::span<std::uint8_t, 8> out,
    std::span<std::string const, 6> words)
{
    // The additional byte is needed for parity
    std::array<std::uint8_t, 9> buf{};
    std::size_t pos = 0;

    for (auto word : words)
    {
        if (word.empty() || word.size() > 4)
            return false;

        word = normalize(std::move(word));

        auto const idx = dictionaryLookup(word);

        if (!idx)
            return false;

        pos += insertBits(buf, *idx, pos);
    }

    if (parityBits(buf) != (buf[8] >> 6))
        return false;

    std::copy_n(buf.data(), out.size(), out.begin());
    return true;
}

}  // namespace

std::optional<std::array<std::uint8_t, 16>>
keyFromEnglish(std::string_view human)
{
    if (human.size() < 23 || human.size() > 128)
        return std::nullopt;

    std::vector<std::string> words;
    std::string trimmed{human};
    boost::algorithm::trim(trimmed);
    boost::algorithm::split(
        words,
        trimmed,
        boost::algorithm::is_space(),
        boost::algorithm::token_compress_on);

    if (words.size() != 12)
        return std::nullopt;

    std::array<std::uint8_t, 16> key{};

    if (!englishToBytes(
            std::span{key}.subspan<0, 8>(), std::span(words).subspan<0, 6>()))
        return std::nullopt;

    if (!englishToBytes(
            std::span{key}.subspan<8, 8>(), std::span(words).subspan<6, 6>()))
        return std::nullopt;

    return key;
}

std::optional<std::string>
englishFromKey(std::span<std::uint8_t const> key)
{
    if (key.size() != 16)
        return std::nullopt;

    std::string result;

    for (auto const w : bytesToEnglish(key.subspan<0, 8>()))
    {
        if (!result.empty())
            result += ' ';

        result += w;
    }

    for (auto const w : bytesToEnglish(key.subspan<8, 8>()))
    {
        result += ' ';
        result += w;
    }

    return result;
}

std::string_view
wordFromBlob(std::span<std::uint8_t const> blob)
{
    // This is a simple implementation of the Jenkins one-at-a-time hash
    // algorithm:
    // http://en.wikipedia.org/wiki/Jenkins_hash_function#one-at-a-time
    std::uint32_t hash = 0;

    for (auto byte : blob)
    {
        hash += byte;
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }

    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);

    return wordlist[hash % std::size(wordlist)];
}

}  // namespace rfc1751
}  // namespace ripple
