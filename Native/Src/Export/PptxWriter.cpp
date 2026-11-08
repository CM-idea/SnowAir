#include "pch.h"
#include "PptxWriter.h"
#include <fstream>
#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>

#pragma pack(push, 1)
namespace {
    struct ZipLocal {
        uint32_t sig{ 0x04034b50 };
        uint16_t ver{ 20 };
        uint16_t flags{ 0 };
        uint16_t method{ 0 };
        uint16_t time{ 0 };
        uint16_t date{ 0 };
        uint32_t crc{ 0 };
        uint32_t csize{ 0 };
        uint32_t usize{ 0 };
        uint16_t nlen{ 0 };
        uint16_t elen{ 0 };
    };
    struct ZipCentral {
        uint32_t sig{ 0x02014b50 };
        uint16_t verMade{ 20 };
        uint16_t verNeed{ 20 };
        uint16_t flags{ 0 };
        uint16_t method{ 0 };
        uint16_t time{ 0 };
        uint16_t date{ 0 };
        uint32_t crc{ 0 };
        uint32_t csize{ 0 };
        uint32_t usize{ 0 };
        uint16_t nlen{ 0 };
        uint16_t elen{ 0 };
        uint16_t clen{ 0 };
        uint16_t disk{ 0 };
        uint16_t iattr{ 0 };
        uint32_t eattr{ 0 };
        uint32_t off{ 0 };
    };
    struct ZipEnd {
        uint32_t sig{ 0x06054b50 };
        uint16_t disk{ 0 };
        uint16_t cdisk{ 0 };
        uint16_t n1{ 0 };
        uint16_t n2{ 0 };
        uint32_t csize{ 0 };
        uint32_t coff{ 0 };
        uint16_t clen{ 0 };
    };
}
#pragma pack(pop)

namespace {
    uint32_t crc32(const std::vector<BYTE>& data)
    {
        static uint32_t table[256]{};
        static bool init{ false };
        if (!init) {
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                table[i] = c;
            }
            init = true;
        }
        uint32_t c = 0xFFFFFFFFu;
        for (auto b : data)
            c = table[(c ^ b) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }

    struct Entry {
        std::string name;
        std::vector<BYTE> data;
    };

    void addText(std::vector<Entry>& files, const std::string& name, const std::string& text)
    {
        Entry e;
        e.name = name;
        e.data.assign(text.begin(), text.end());
        files.push_back(std::move(e));
    }

    std::string emu(int px)
    {
        return std::to_string((long long)px * 9525LL);
    }

    std::string slideXml(const std::string& rid, int x, int y, int cx, int cy)
    {
        std::string out;
        out += "<p:sld xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
            " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
            " xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\">";
        out += "<p:cSld><p:spTree>";
        out += "<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>";
        out += "<p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/>"
            "<a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>";
        out += "<p:pic><p:nvPicPr><p:cNvPr id=\"2\" name=\"Picture 1\"/>"
            "<p:cNvPicPr><a:picLocks noChangeAspect=\"1\"/></p:cNvPicPr><p:nvPr/></p:nvPicPr>";
        out += "<p:blipFill><a:blip r:embed=\"" + rid + "\"/><a:stretch><a:fillRect/></a:stretch></p:blipFill>";
        out += "<p:spPr><a:xfrm><a:off x=\"" + std::to_string(x) + "\" y=\"" + std::to_string(y) + "\"/>"
            "<a:ext cx=\"" + std::to_string(cx) + "\" cy=\"" + std::to_string(cy) + "\"/></a:xfrm>"
            "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic>";
        out += "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sld>";
        return out;
    }

    std::string emptySpTree()
    {
        return "<p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
            "<p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/>"
            "<a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr></p:spTree>";
    }

    const char* kTheme = R"(<a:theme xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" name="Office Theme">
<a:themeElements>
<a:clrScheme name="Office">
<a:dk1><a:srgbClr val="000000"/></a:dk1><a:lt1><a:srgbClr val="FFFFFF"/></a:lt1>
<a:dk2><a:srgbClr val="44546A"/></a:dk2><a:lt2><a:srgbClr val="E7E6E6"/></a:lt2>
<a:accent1><a:srgbClr val="4472C4"/></a:accent1><a:accent2><a:srgbClr val="ED7D31"/></a:accent2>
<a:accent3><a:srgbClr val="A5A5A5"/></a:accent3><a:accent4><a:srgbClr val="FFC000"/></a:accent4>
<a:accent5><a:srgbClr val="5B9BD5"/></a:accent5><a:accent6><a:srgbClr val="70AD47"/></a:accent6>
<a:hlink><a:srgbClr val="0563C1"/></a:hlink><a:folHlink><a:srgbClr val="954F72"/></a:folHlink>
</a:clrScheme>
<a:fontScheme name="Office">
<a:majorFont><a:latin typeface="Calibri Light"/><a:ea typeface=""/><a:cs typeface=""/></a:majorFont>
<a:minorFont><a:latin typeface="Calibri"/><a:ea typeface=""/><a:cs typeface=""/></a:minorFont>
</a:fontScheme>
<a:fmtScheme name="Office">
<a:fillStyleLst>
<a:solidFill><a:schemeClr val="phClr"/></a:solidFill>
<a:solidFill><a:schemeClr val="phClr"/></a:solidFill>
<a:solidFill><a:schemeClr val="phClr"/></a:solidFill>
</a:fillStyleLst>
<a:lnStyleLst>
<a:ln w="6350"><a:solidFill><a:schemeClr val="phClr"/></a:solidFill></a:ln>
<a:ln w="12700"><a:solidFill><a:schemeClr val="phClr"/></a:solidFill></a:ln>
<a:ln w="19050"><a:solidFill><a:schemeClr val="phClr"/></a:solidFill></a:ln>
</a:lnStyleLst>
<a:effectStyleLst>
<a:effectStyle><a:effectLst/></a:effectStyle>
<a:effectStyle><a:effectLst/></a:effectStyle>
<a:effectStyle><a:effectLst/></a:effectStyle>
</a:effectStyleLst>
<a:bgFillStyleLst>
<a:solidFill><a:schemeClr val="phClr"/></a:solidFill>
<a:solidFill><a:schemeClr val="phClr"/></a:solidFill>
<a:solidFill><a:schemeClr val="phClr"/></a:solidFill>
</a:bgFillStyleLst>
</a:fmtScheme>
</a:themeElements>
</a:theme>)";
}

void PptxWriter::addPage(std::vector<BYTE> jpeg, int pixelW, int pixelH)
{
    if (pixelW <= 0 || pixelH <= 0 || jpeg.empty()) return;
    pages_.push_back({ std::move(jpeg), pixelW, pixelH });
}

bool PptxWriter::save(const std::wstring& path)
{
    if (pages_.empty()) return false;

    int slideW = pages_[0].w;
    int slideH = pages_[0].h;
    int n = (int)pages_.size();

    std::vector<Entry> files;

    std::string ct = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Default Extension=\"jpeg\" ContentType=\"image/jpeg\"/>"
        "<Override PartName=\"/ppt/presentation.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml\"/>"
        "<Override PartName=\"/ppt/slideMasters/slideMaster1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slideMaster+xml\"/>"
        "<Override PartName=\"/ppt/slideLayouts/slideLayout1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slideLayout+xml\"/>"
        "<Override PartName=\"/ppt/theme/theme1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.theme+xml\"/>";
    for (int i = 0; i < n; ++i)
        ct += "<Override PartName=\"/ppt/slides/slide" + std::to_string(i + 1) +
            ".xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slide+xml\"/>";
    ct += "</Types>";
    addText(files, "[Content_Types].xml", ct);

    addText(files, "_rels/.rels",
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"ppt/presentation.xml\"/>"
        "</Relationships>");

    std::string presRels = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster\" Target=\"slideMasters/slideMaster1.xml\"/>";
    std::string sldIds;
    for (int i = 0; i < n; ++i) {
        presRels += "<Relationship Id=\"rId" + std::to_string(i + 2) +
            "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide\" Target=\"slides/slide" +
            std::to_string(i + 1) + ".xml\"/>";
        sldIds += "<p:sldId id=\"" + std::to_string(256 + i) + "\" r:id=\"rId" + std::to_string(i + 2) + "\"/>";
    }
    presRels += "</Relationships>";
    addText(files, "ppt/_rels/presentation.xml.rels", presRels);

    std::string pres = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<p:presentation xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
        " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
        " xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\">"
        "<p:sldMasterIdLst><p:sldMasterId id=\"2147483648\" r:id=\"rId1\"/></p:sldMasterIdLst>"
        "<p:sldIdLst>" + sldIds + "</p:sldIdLst>"
        "<p:sldSz cx=\"" + emu(slideW) + "\" cy=\"" + emu(slideH) + "\" type=\"custom\"/>"
        "<p:notesSz cx=\"6858000\" cy=\"9144000\"/>"
        "</p:presentation>";
    addText(files, "ppt/presentation.xml", pres);

    std::string master = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<p:sldMaster xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
        " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
        " xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\">"
        "<p:cSld>" + emptySpTree() + "</p:cSld>"
        "<p:clrMap bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" tx2=\"dk2\""
        " accent1=\"accent1\" accent2=\"accent2\" accent3=\"accent3\" accent4=\"accent4\""
        " accent5=\"accent5\" accent6=\"accent6\" hlink=\"hlink\" folHlink=\"folHlink\"/>"
        "<p:sldLayoutIdLst><p:sldLayoutId id=\"2147483649\" r:id=\"rId1\"/></p:sldLayoutIdLst>"
        "<p:txStyles><p:titleStyle><a:lvl1pPr><a:defRPr/></a:lvl1pPr></p:titleStyle>"
        "<p:bodyStyle><a:lvl1pPr><a:defRPr/></a:lvl1pPr></p:bodyStyle>"
        "<p:otherStyle><a:lvl1pPr><a:defRPr/></a:lvl1pPr></p:otherStyle></p:txStyles>"
        "</p:sldMaster>";
    addText(files, "ppt/slideMasters/slideMaster1.xml", master);
    addText(files, "ppt/slideMasters/_rels/slideMaster1.xml.rels",
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout\" Target=\"../slideLayouts/slideLayout1.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme\" Target=\"../theme/theme1.xml\"/>"
        "</Relationships>");

    std::string layout = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<p:sldLayout xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
        " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
        " xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\""
        " type=\"blank\" preserve=\"1\"><p:cSld name=\"Blank\">" + emptySpTree() +
        "</p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sldLayout>";
    addText(files, "ppt/slideLayouts/slideLayout1.xml", layout);
    addText(files, "ppt/slideLayouts/_rels/slideLayout1.xml.rels",
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster\" Target=\"../slideMasters/slideMaster1.xml\"/>"
        "</Relationships>");

    addText(files, "ppt/theme/theme1.xml", kTheme);

    for (int i = 0; i < n; ++i) {
        auto& p = pages_[i];
        double scale = std::min((double)slideW / p.w, (double)slideH / p.h);
        int cx = (int)(p.w * scale * 9525);
        int cy = (int)(p.h * scale * 9525);
        int x = (int)(((long long)slideW * 9525 - cx) / 2);
        int y = (int)(((long long)slideH * 9525 - cy) / 2);
        addText(files, "ppt/slides/slide" + std::to_string(i + 1) + ".xml",
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>" +
            slideXml("rId2", x, y, cx, cy));
        addText(files, "ppt/slides/_rels/slide" + std::to_string(i + 1) + ".xml.rels",
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
            "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout\" Target=\"../slideLayouts/slideLayout1.xml\"/>"
            "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"../media/image" +
            std::to_string(i + 1) + ".jpeg\"/>"
            "</Relationships>");

        Entry img;
        img.name = "ppt/media/image" + std::to_string(i + 1) + ".jpeg";
        img.data = p.jpeg;
        files.push_back(std::move(img));
    }

    // 写 ZIP（store，不压缩）
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;

    std::vector<ZipCentral> central;
    central.reserve(files.size());
    for (auto& e : files) {
        uint32_t crc = crc32(e.data);
        ZipLocal local;
        local.crc = crc;
        local.csize = local.usize = (uint32_t)e.data.size();
        local.nlen = (uint16_t)e.name.size();
        uint32_t offset = (uint32_t)f.tellp();

        f.write(reinterpret_cast<const char*>(&local), sizeof(local));
        f.write(e.name.data(), (std::streamsize)e.name.size());
        if (!e.data.empty()) f.write(reinterpret_cast<const char*>(e.data.data()), (std::streamsize)e.data.size());

        ZipCentral c;
        c.crc = crc;
        c.csize = c.usize = (uint32_t)e.data.size();
        c.nlen = (uint16_t)e.name.size();
        c.off = offset;
        central.push_back(c);
    }

    uint32_t centralStart = (uint32_t)f.tellp();
    for (size_t i = 0; i < central.size(); ++i) {
        auto& c = central[i];
        f.write(reinterpret_cast<const char*>(&c), sizeof(c));
        f.write(files[i].name.data(), (std::streamsize)files[i].name.size());
    }
    uint32_t centralSize = (uint32_t)f.tellp() - centralStart;

    ZipEnd end;
    end.n1 = end.n2 = (uint32_t)files.size();
    end.csize = centralSize;
    end.coff = centralStart;
    f.write(reinterpret_cast<const char*>(&end), sizeof(end));
    f.close();
    return true;
}
