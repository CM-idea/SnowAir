#include "pch.h"
#include "PdfWriter.h"
#include <fstream>
#include <sstream>

namespace {
    std::string fmtNum(double v)
    {
        std::ostringstream ss;
        ss.setf(std::ios::fixed);
        ss.precision(2);
        ss << v;
        std::string s = ss.str();
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
        return s;
    }
}

void PdfWriter::addPage(std::vector<BYTE> jpeg, int pixelW, int pixelH)
{
    if (pixelW <= 0 || pixelH <= 0 || jpeg.empty()) return;
    pages_.push_back({ std::move(jpeg), pixelW, pixelH });
}

bool PdfWriter::save(const std::wstring& path)
{
    if (pages_.empty()) return false;

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;

    int n = (int)pages_.size();
    int pageObjBase = 3;
    int objCount = 2 + n * 3;
    std::vector<long long> offsets(objCount + 1, 0);
    auto beginObj = [&](int id) { offsets[id] = (long long)f.tellp(); };

    f << "%PDF-1.4\n";

    std::ostringstream kids;
    for (int i = 0; i < n; ++i) {
        if (i) kids << ' ';
        kids << (pageObjBase + i * 3) << " 0 R";
    }

    beginObj(1);
    f << "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
    beginObj(2);
    f << "2 0 obj\n<< /Type /Pages /Kids [" << kids.str() << "] /Count " << n << " >>\nendobj\n";

    for (int i = 0; i < n; ++i) {
        int pageId = pageObjBase + i * 3;
        int contentId = pageId + 1;
        int imageId = pageId + 2;
        double pw = pages_[i].w * 0.75;
        double ph = pages_[i].h * 0.75;

        beginObj(pageId);
        f << pageId << " 0 obj\n"
            << "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " << fmtNum(pw) << " " << fmtNum(ph) << "]"
            << " /Resources << /XObject << /Im0 " << imageId << " 0 R >> >>"
            << " /Contents " << contentId << " 0 R >>\nendobj\n";

        std::string content = "q\n" + fmtNum(pw) + " 0 0 " + fmtNum(ph) + " 0 0 cm\n/Im0 Do\nQ\n";
        beginObj(contentId);
        f << contentId << " 0 obj\n<< /Length " << content.size() << " >>\nstream\n"
            << content << "endstream\nendobj\n";

        beginObj(imageId);
        f << imageId << " 0 obj\n"
            << "<< /Type /XObject /Subtype /Image /Width " << pages_[i].w
            << " /Height " << pages_[i].h
            << " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length "
            << pages_[i].jpeg.size() << " >>\nstream\n";
        f.write(reinterpret_cast<const char*>(pages_[i].jpeg.data()), (std::streamsize)pages_[i].jpeg.size());
        f << "\nendstream\nendobj\n";
    }

    long long xrefPos = (long long)f.tellp();
    f << "xref\n0 " << (objCount + 1) << "\n";
    f << "0000000000 65535 f \n";
    for (int i = 1; i <= objCount; ++i) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%010lld 00000 n \n", offsets[i]);
        f << buf;
    }
    f << "trailer\n<< /Size " << (objCount + 1) << " /Root 1 0 R >>\nstartxref\n"
        << xrefPos << "\n%%EOF\n";
    f.close();
    return true;
}
