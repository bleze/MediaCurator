#include "ui/McLanguageFlags.h"

#include <QHash>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPixmapCache>
#include <QSvgRenderer>

namespace {

struct LangEntry {
	const char* codes;   // comma-separated ISO 639 variants (639-1, 639-2/B, 639-2/T)
	const char* country; // ISO 3166-1 alpha-2 (flag resource name)
	const char* name;    // English display name
};

// Movie-centric mapping: each language points at the flag viewers expect for
// it, not at every country where it is spoken.
static const LangEntry kLangTable[] = {
	{ "en,eng",               "gb", "English"    },
	{ "da,dan",               "dk", "Danish"     },
	{ "sv,swe",               "se", "Swedish"    },
	{ "no,nor,nb,nob,nn,nno", "no", "Norwegian"  },
	{ "fi,fin",               "fi", "Finnish"    },
	{ "is,isl,ice",           "is", "Icelandic"  },
	{ "de,deu,ger",           "de", "German"     },
	{ "fr,fra,fre",           "fr", "French"     },
	{ "es,spa",               "es", "Spanish"    },
	{ "it,ita",               "it", "Italian"    },
	{ "pt,por",               "pt", "Portuguese" },
	{ "pb,pob",               "br", "Portuguese (Brazil)" },
	{ "nl,nld,dut",           "nl", "Dutch"      },
	{ "ja,jpn",               "jp", "Japanese"   },
	{ "ko,kor",               "kr", "Korean"     },
	{ "zh,zho,chi,cmn",       "cn", "Chinese"    },
	{ "yue",                  "hk", "Cantonese"  },
	{ "ru,rus",               "ru", "Russian"    },
	{ "pl,pol",               "pl", "Polish"     },
	{ "cs,ces,cze",           "cz", "Czech"      },
	{ "sk,slk,slo",           "sk", "Slovak"     },
	{ "hu,hun",               "hu", "Hungarian"  },
	{ "tr,tur",               "tr", "Turkish"    },
	{ "ar,ara",               "sa", "Arabic"     },
	{ "he,heb",               "il", "Hebrew"     },
	{ "hi,hin",               "in", "Hindi"      },
	{ "ta,tam",               "in", "Tamil"      },
	{ "te,tel",               "in", "Telugu"     },
	{ "ml,mal",               "in", "Malayalam"  },
	{ "kn,kan",               "in", "Kannada"    },
	{ "mr,mar",               "in", "Marathi"    },
	{ "pa,pan",               "in", "Punjabi"    },
	{ "gu,guj",               "in", "Gujarati"   },
	{ "bn,ben",               "bd", "Bengali"    },
	{ "th,tha",               "th", "Thai"       },
	{ "vi,vie",               "vn", "Vietnamese" },
	{ "id,ind",               "id", "Indonesian" },
	{ "ms,msa,may",           "my", "Malay"      },
	{ "tl,tgl,fil",           "ph", "Filipino"   },
	{ "el,ell,gre",           "gr", "Greek"      },
	{ "ro,ron,rum",           "ro", "Romanian"   },
	{ "bg,bul",               "bg", "Bulgarian"  },
	{ "uk,ukr",               "ua", "Ukrainian"  },
	{ "sr,srp",               "rs", "Serbian"    },
	{ "hr,hrv",               "hr", "Croatian"   },
	{ "bs,bos",               "ba", "Bosnian"    },
	{ "sl,slv",               "si", "Slovenian"  },
	{ "mk,mkd,mac",           "mk", "Macedonian" },
	{ "sq,sqi,alb",           "al", "Albanian"   },
	{ "et,est",               "ee", "Estonian"   },
	{ "lv,lav",               "lv", "Latvian"    },
	{ "lt,lit",               "lt", "Lithuanian" },
	{ "fa,fas,per",           "ir", "Persian"    },
	{ "ur,urd",               "pk", "Urdu"       },
	{ "af,afr",               "za", "Afrikaans"  },
	{ "ga,gle",               "ie", "Irish"      },
	{ "be,bel",               "by", "Belarusian" },
	{ "kk,kaz",               "kz", "Kazakh"     },
	{ "az,aze",               "az", "Azerbaijani"},
	{ "ka,kat,geo",           "ge", "Georgian"   },
	{ "hy,hye,arm",           "am", "Armenian"   },
};

struct LangInfo { QString country; QString name; };

static const QHash<QString, LangInfo>& langMap()
{
	static const QHash<QString, LangInfo> map = [] {
		QHash<QString, LangInfo> m;
		for (const LangEntry& e : kLangTable) {
			const LangInfo info{ QString::fromLatin1(e.country), QString::fromLatin1(e.name) };
			const QStringList codes = QString::fromLatin1(e.codes).split(QLatin1Char(','));
			for (const QString& code : codes)
				m.insert(code, info);
		}
		return m;
	}();
	return map;
}

static const QHash<QString, QString>& iso1Map()
{
	static const QHash<QString, QString> map = [] {
		QHash<QString, QString> m;
		for (const LangEntry& e : kLangTable) {
			const QStringList codes = QString::fromLatin1(e.codes).split(QLatin1Char(','));
			if (codes.isEmpty()) continue;
			const QString iso1 = codes.first(); // first entry is always ISO 639-1
			for (const QString& code : codes)
				m.insert(code, iso1);
		}
		return m;
	}();
	return map;
}

} // anonymous namespace

namespace Mc {
namespace McLanguageFlags {

QString countryForLanguage(const QString& lang)
{
	const auto it = langMap().constFind(lang.toLower().trimmed());
	return it == langMap().constEnd() ? QString() : it->country;
}

QString displayName(const QString& lang)
{
	const auto it = langMap().constFind(lang.toLower().trimmed());
	return it == langMap().constEnd() ? lang : it->name;
}

QString toIso1(const QString& langCode)
{
	return iso1Map().value(langCode.toLower().trimmed());
}

QPixmap flag(const QString& lang, int height, qreal dpr)
{
	const QString cc = countryForLanguage(lang);
	if (cc.isEmpty() || height <= 0) return {};

	const int     h   = qMax(1, qRound(height * dpr));
	const int     w   = h * 4 / 3;
	const QString key = QStringLiteral("mc_flag/%1/%2").arg(cc).arg(h);

	QPixmap pm;
	if (!QPixmapCache::find(key, &pm)) {
		QSvgRenderer renderer(QStringLiteral(":/flags/%1.svg").arg(cc));
		if (!renderer.isValid()) return {};

		QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
		img.fill(Qt::transparent);
		QPainter p(&img);
		p.setRenderHint(QPainter::Antialiasing);
		// Rounded clip + hairline border so light flag fields (e.g. Japan)
		// stay readable against the coloured badge pill behind them.
		QPainterPath clip;
		clip.addRoundedRect(QRectF(0, 0, w, h), 2 * dpr, 2 * dpr);
		p.setClipPath(clip);
		renderer.render(&p, QRectF(0, 0, w, h));
		p.setClipping(false);
		p.setPen(QPen(QColor(0, 0, 0, 70), 1));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(QRectF(0.5, 0.5, w - 1.0, h - 1.0), 2 * dpr, 2 * dpr);
		p.end();
		pm = QPixmap::fromImage(img);
		QPixmapCache::insert(key, pm);
	}
	pm.setDevicePixelRatio(dpr);
	return pm;
}

} // namespace McLanguageFlags
} // namespace Mc
