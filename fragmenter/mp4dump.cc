/*
 * 
 * copyright (c) 2010 ZAO Inventos (inventos.ru)
 * copyright (c) 2010 jk
 *
 * This file is part of VideoCycle.
 *
 * VideoCycle is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * VideoCycle is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include "mp4.hh"
#include "base64.hh"
#include <boost/program_options.hpp>
#include <sstream>
#include <fstream>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
    std::string manifest_name = "manifest.f4m";
    std::string docroot = ".";
    std::string basedir = ".";
    std::string fragments_dir("samples");
    std::string video_id("some_video");
    std::string srcfile;
}


void write16(std::streambuf& buf, uint16_t value) {
    buf.sputc( (value >> 8) & 0xFF );
    buf.sputc( value & 0xFF );
}

void write24(std::streambuf& buf, uint32_t value) {
    buf.sputc( (value >> 16) & 0xFF );
    buf.sputc( (value >> 8) & 0xFF );
    buf.sputc( value & 0xFF );
}

void write32(std::streambuf& buf, uint32_t value) {
    buf.sputc( (value >> 24) & 0xFF );
    buf.sputc( (value >> 16) & 0xFF );
    buf.sputc( (value >> 8) & 0xFF );
    buf.sputc( value & 0xFF );
}

void write64(std::streambuf& buf, uint64_t value) {
    buf.sputc( (value >> 56) & 0xFF );
    buf.sputc( (value >> 48) & 0xFF );
    buf.sputc( (value >> 40) & 0xFF );
    buf.sputc( (value >> 32) & 0xFF );
    buf.sputc( (value >> 24) & 0xFF );
    buf.sputc( (value >> 16) & 0xFF );
    buf.sputc( (value >> 8) & 0xFF );
    buf.sputc( value & 0xFF );
}

void writebox(std::streambuf& buf, const char *name, const std::string& box) {
    write32(buf, box.size() + 8);
    buf.sputn(name, 4);
    buf.sputn(box.c_str(), box.size());
}



void *mapping;

struct Fragment {
    double _ts;
    double _dur;
    Fragment(unsigned ts, unsigned dur) : _ts(ts), _dur(dur) {}
};


void close_fragment(unsigned segment, unsigned fragment, const std::string& fragdata, std::vector<Fragment>& fragments, double ts, double duration) {
    if ( fragdata.size() ) {
        std::stringstream filename;
        filename << docroot << '/' << basedir << '/' << fragments_dir << '/' << "Seg" << segment << "-Frag" << fragment;
        size_t fragment_size = fragdata.size() + 8;
        char prefix[8];
        prefix[0] = (fragment_size >> 24) & 0xFF;
        prefix[1] = (fragment_size >> 16) & 0xFF;
        prefix[2] = (fragment_size >> 8) & 0xFF;
        prefix[3] = fragment_size & 0xFF;
        prefix[4] = 'm';
        prefix[5] = 'd';
        prefix[6] = 'a';
        prefix[7] = 't';

        std::ofstream out(filename.str().c_str());
        std::cerr << filename.str() << std::endl;
        assert (out);

        out.write(prefix, 8);
        out.write(fragdata.c_str(), fragdata.size());

        out.close();
        fragments.push_back(Fragment(ts, duration));
        assert(duration != 0);
    }
}

std::vector<Fragment> write_fragments(const ::boost::shared_ptr<mp4::Context>& ctx, double timelimit) {
    unsigned segment = 1;
    unsigned fragment = 1;
    double limit_ts = timelimit;
    double old_ts = 0;
    double now;
    unsigned nsample = 0;
    std::vector<Fragment> fragments;
    std::stringbuf sb;

    if ( ctx->has_video() ) {
        sb.sputc(9);
        auto ve = ctx->videoextra();
        write24(sb, ve.size() + 5);
        write32(sb, 0);
        write24(sb, 0);
        sb.sputn("\x17\0\0\0\0", 5);
        sb.sputn(ve.c_str(), ve.size());
        write32(sb, ve.size() + 16);
    }
    if ( ctx->has_audio() ) {
        sb.sputc(8);
        auto ae = ctx->audioextra();
        write24(sb, ae.size() + 2);
        write32(sb, 0);
        write24(sb, 0);
        sb.sputn("\xaf\0", 2);
        sb.sputn(ae.c_str(), ae.size());
        write32(sb, ae.size() + 13);
    }

    while ( nsample < ctx->nsamples() ) {
        std::cerr << "nsample=" << nsample << ", ";
        mp4::SampleInfo *si = ctx->get_sample(nsample++);
        now = si->timestamp();
        std::cerr << "timestamp=" << now << ", ";

        unsigned total = 11 + si->_sample_size;
        if ( si->_video ) {
            if ( si->_keyframe && now >= limit_ts ) {
                std::cerr << "close: " << fragment << "\n";
                close_fragment(segment, fragment++, sb.str(), fragments, old_ts, now - old_ts);
                old_ts = now;
                limit_ts = now + timelimit;
                sb.str(std::string());
            }
            sb.sputc(9);
            write24(sb, si->_sample_size + 5);
            total += 5;
        }
        else {
            sb.sputc(8);
            write24(sb, si->_sample_size + 2);
            total += 2;
        }
        unsigned uts = now * 1000;
        write24(sb, uts); sb.sputc( (uts >> 24) & 0xff );
        std::cerr << "timestamp_24=" << std::hex << uts << "\n";
        write24(sb, 0);
        if ( si->_video ) {
            if ( si->_keyframe ) {
                sb.sputn("\x17\x1", 2);
            }
            else {
                sb.sputn("\x27\x1", 2);
            }
            write24(sb, si->_composition_offset * 1000.0 / si->_timescale); // composition time offset
        }
        else {
            sb.sputn("\xaf\x1", 2);
        }
        sb.sputn((char*)mapping + si->_offset, si->_sample_size);
        write32(sb, total);
    }
    close_fragment(segment, fragment, sb.str(), fragments, now, ctx->duration() - now);
    return fragments;
}


void ok(const ::boost::shared_ptr<mp4::Context>& ctx) { 
    std::cout << ctx->_samples.size() << " samples found.\n";
    std::cout << "\nnsamples: " << ctx->nsamples()
      << "\nhas video: " << (ctx->has_video() ? "yes" : "no")
      << "\nhas audio: " << (ctx->has_audio() ? "yes" : "no")
      << "\nwidth: " << ctx->width()
      << "\nheight: " << ctx->height()
      << "\npwidth: " << ctx->pwidth()
      << "\npheight: " << ctx->pheight()
      << "\nbitrate: " << ctx->bitrate()
      << "\nduration: " << ctx->duration()
      << std::endl;

    std::cout << "writing..." << std::endl;
    std::vector<Fragment> fragments = write_fragments(ctx, 10); 
    // int bootstrap = open("bootstrap", O_CREAT|O_WRONLY, 0644);

    std::stringbuf abst;
    abst.sputc(0); // version
    abst.sputn("\0\0", 3); // flags;
    write32(abst, 14); // bootstrapinfoversion
    abst.sputc(0); // profile, live, update
    write32(abst, 1000); // timescale
    write64(abst, ctx->duration() * 1000); // currentmediatime
    write64(abst, 0); // smptetimecodeoffset
    abst.sputc(0); // movieidentifier
    abst.sputc(0); // serverentrycount
    abst.sputc(0); // qualityentrycount
    abst.sputc(0); // drmdata
    abst.sputc(0); // metadata
    abst.sputc(1); // segment run table count

    std::stringbuf asrt;
    asrt.sputc(0); // version
    write24(asrt, 0); // flags
    asrt.sputc(0); // qualityentrycount
    write32(asrt, 1); // segmentrunentry count
    write32(asrt, 1); // first segment
    write32(asrt, fragments.size()); // fragments per segment

    writebox(abst, "asrt", asrt.str());

    abst.sputc(1); // fragment run table count

    std::stringbuf afrt;
    afrt.sputc(0); // version
    write24(afrt, 0); // flags
    write32(afrt, 1000); // timescale
    afrt.sputc(0); // qualityentrycount
    std::cerr << fragments.size() << " fragments\n";
    write32(afrt, fragments.size()); // fragmentrunentrycount
    for ( unsigned ifragment = 0; ifragment < fragments.size(); ++ifragment ) {
        write32(afrt, ifragment + 1);
        write64(afrt, uint64_t(fragments[ifragment]._ts * 1000));
        write32(afrt, uint32_t(fragments[ifragment]._dur * 1000));
        if ( fragments[ifragment]._dur == 0 ) {
            assert ( ifragment == fragments.size() - 1 );
            afrt.sputc(0);
        }
    }
    writebox(abst, "afrt", afrt.str());

    std::stringbuf bootstrapinfo_stream;
    writebox(bootstrapinfo_stream, "abst", abst.str());
    std::string bootstrapinfo = bootstrapinfo_stream.str();

    std::string info("bt");
    std::stringstream manifestname;
    manifestname << docroot << '/' << basedir << '/' << manifest_name;
    std::ofstream manifest_out(manifestname.str().c_str());
    manifest_out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<manifest xmlns=\"http://ns.adobe.com/f4m/1.0\">\n"
      "<id>" << video_id << "</id>\n"
      "<streamType>recorded</streamType>\n"
      "<duration>" << ctx->duration() << "</duration>\n"
      "<bootstrapInfo profile=\"named\" id=\"" << info << "\">";
    base64::encode(manifest_out.rdbuf(), bootstrapinfo.c_str(), bootstrapinfo.size());
    manifest_out << "</bootstrapinfo>\n"
      "<media streamId=\"" << video_id << "\" url=\"" << fragments_dir << '/' << "\" bootstrapinfoId=\"" << info << "\" />\n"
      "</manifest>\n";
    manifest_out.close();
}


void parse_options(int argc, char **argv) {
    namespace po = boost::program_options;

    po::options_description desc("Allowed options");
    desc.add_options()
      ("help", "produce help message")
      ("src", po::value<std::string>(&srcfile), "source mp4 file name")
      ("docroot", po::value<std::string>(&docroot)->default_value("."), "docroot directory")
      ("basedir", po::value<std::string>(&basedir)->default_value("."), "base directory for manifest file")
      ("fragments", po::value<std::string>(&fragments_dir)->default_value("samples"), "directory for fragment files")
      ("video_id", po::value<std::string>(&video_id)->default_value("some_video"), "video id for manifest file")
      ("manifest", po::value<std::string>(&manifest_name)->default_value("manifest.f4m"), "manifest file name")
    ;

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);    

    if (vm.count("help") || argc == 1 || srcfile.size() == 0) {
        std::cerr << desc << "\n";
        exit(1);
    }
    
}



int main(int argc, char **argv) {
    // reactor::set_timer(::boost::bind(run_parser, argv[1]), 1000);
    parse_options(argc, argv);
    struct stat st;
    assert ( stat(srcfile.c_str(), &st) != -1 );
    int fd = open(srcfile.c_str(), O_RDONLY);
    mapping = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    assert ( mapping != (void*)(-1) );

    boost::shared_ptr<mp4::Context> ctxt(new mp4::Context);
    off_t offset = 0;
    while ( unsigned nbytes = ctxt->wants() ) {
        if ( unsigned s = ctxt->to_skip() ) {
            offset += s;
            ctxt->skip(0);                    
        }
        if ( offset >= st.st_size ) {
            break;
        }
        ctxt->feed( (char*)mapping + offset, nbytes );
        offset += nbytes;
    }
    ctxt->finalize();
    ok(ctxt);
}
