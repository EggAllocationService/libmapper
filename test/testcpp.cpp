
#include <cstring>
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <array>

#include "config.h"

#include <mpr/mpr_cpp.h>

#ifdef HAVE_ARPA_INET_H
 #include <arpa/inet.h>
#endif

using namespace mpr;

int received = 0;

int verbose = 1;
int terminate = 0;

class out_stream : public std::ostream {
public:
    out_stream() : std::ostream (&buf) {}

private:
    class null_out_buf : public std::streambuf {
    public:
        virtual std::streamsize xsputn (const char * s, std::streamsize n) {
            return n;
        }
        virtual int overflow (int c) {
            return 1;
        }
    };
    null_out_buf buf;
};
out_stream null_out;

void handler(mpr_sig sig, mpr_sig_evt event, mpr_id instance, int length,
             mpr_type type, const void *value, mpr_time t)
{
    ++received;
    if (!value || !verbose)
        return;

    const char *name = mpr_obj_get_prop_str(sig, MPR_PROP_NAME, NULL);
    printf("--> destination got %s", name);

    switch (type) {
        case MPR_INT32: {
            int *v = (int*)value;
            for (int i = 0; i < length; i++) {
                printf(" %d", v[i]);
            }
            break;
        }
        case MPR_FLT: {
            float *v = (float*)value;
            for (int i = 0; i < length; i++) {
                printf(" %f", v[i]);
            }
            break;
        }
        case MPR_DBL: {
            double *v = (double*)value;
            for (int i = 0; i < length; i++) {
                printf(" %f", v[i]);
            }
            break;
        }
        default:
            break;
    }
    printf("\n");
}

int main(int argc, char ** argv)
{
    unsigned int i = 0, j, result = 0;

    // process flags for -v verbose, -t terminate, -h help
    for (i = 1; i < argc; i++) {
        if (argv[i] && argv[i][0] == '-') {
            int len = strlen(argv[i]);
            for (j = 1; j < len; j++) {
                switch (argv[i][j]) {
                    case 'h':
                        printf("testcpp.cpp: possible arguments "
                               "-q quiet (suppress output), "
                               "-t terminate automatically, "
                               "-h help\n");
                        return 1;
                        break;
                    case 'q':
                        verbose = 0;
                        break;
                    case 't':
                        terminate = 1;
                        break;
                    default:
                        break;
                }
            }
        }
    }

    std::ostream& out = verbose ? std::cout : null_out;

    Device dev("mydevice");

    // make a copy of the device to check reference counting
//    Device devcopy(dev);

    Signal sig = dev.add_sig(MPR_DIR_IN, 1, "in1", 1, MPR_FLT, "meters",
                             0, 0, handler);
    dev.remove_sig(sig);
    dev.add_sig(MPR_DIR_IN, 1, "in2", 2, MPR_INT32, 0, 0, 0, handler);
    dev.add_sig(MPR_DIR_IN, 1, "in3", 2, MPR_INT32, 0, 0, 0, handler);
    dev.add_sig(MPR_DIR_IN, 1, "in4", 2, MPR_INT32, 0, 0, 0, handler);

    sig = dev.add_sig(MPR_DIR_OUT, 1, "out1", 1, MPR_FLT, "na");
    dev.remove_sig(sig);
    sig = dev.add_sig(MPR_DIR_OUT, 1, "out2", 3, MPR_DBL, "meters");

    out << "waiting" << std::endl;
    while (!dev.ready()) {
        dev.poll(100);
    }
    out << "ready" << std::endl;

    out << "device " << dev[MPR_PROP_NAME] << " ready..." << std::endl;
    out << "  ordinal: " << dev["ordinal"] << std::endl;
    out << "  id: " << dev[MPR_PROP_ID] << std::endl;
    out << "  interface: " << dev.graph().iface() << std::endl;
    out << "  bus url: " << dev.graph().address() << std::endl;
    out << "  port: " << dev["port"] << std::endl;
    out << "  num_inputs: " << dev.signals(MPR_DIR_IN).length() << std::endl;
    out << "  num_outputs: " << dev.signals(MPR_DIR_OUT).length() << std::endl;
    out << "  num_incoming_maps: " << dev.signals().maps(MPR_DIR_IN).length()
        << std::endl;
    out << "  num_outgoing_maps: " << dev.signals().maps(MPR_DIR_OUT).length()
        << std::endl;

    int value[] = {1,2,3,4,5,6};
    dev.set_prop("foo", 6, value);
    out << "foo: " << dev["foo"] << std::endl;

    // test std::array<std::string>
    out << "set and get std::array<std::string>: ";
    std::array<std::string, 3> a1 = {{"one", "two", "three"}};
    dev.set_prop("foo", a1);
    const std::array<std::string, 8> a2 = dev["foo"];
    for (i = 0; i < 8; i++)
        out << a2[i] << " ";
    out << std::endl;

    // test std::array<const char*>
    out << "set and get std::array<const char*>: ";
    std::array<const char*, 3> a3 = {{"four", "five", "six"}};
    dev.set_prop("foo", a3);
    std::array<const char*, 3> a4 = dev["foo"];
    for (i = 0; i < a4.size(); i++)
        out << a4[i] << " ";
    out << std::endl;

    // test plain array of const char*
    out << "set and get const char*[]: ";
    const char* a5[3] = {"seven", "eight", "nine"};
    dev.set_prop("foo", 3, a5);
    const char **a6 = dev["foo"];
    out << a6[0] << " " << a6[1] << " " << a6[2] << std::endl;

    // test plain array of float
    out << "set and get float[]: ";
    float a7[3] = {7.7f, 8.8f, 9.9f};
    dev.set_prop("foo", 3, a7);
    const float *a8 = dev["foo"];
    out << a8[0] << " " << a8[1] << " " << a8[2] << std::endl;

    // test std::vector<const char*>
    out << "set and get std::vector<const char*>: ";
    const char *a9[3] = {"ten", "eleven", "twelve"};
    std::vector<const char*> v1(a9, std::end(a9));
    dev.set_prop("foo", v1);
    std::vector<const char*> v2 = dev["foo"];
    out << "foo: ";
    for (std::vector<const char*>::iterator it = v2.begin(); it != v2.end(); ++it)
        out << *it << " ";
    out << std::endl;

    // test std::vector<std::string>
    out << "set and get std::vector<std::string>: ";
    const char *a10[3] = {"thirteen", "14", "15"};
    std::vector<std::string> v3(a10, std::end(a10));
    dev.set_prop("foo", v3);
    std::vector<std::string> v4 = dev["foo"];
    out << "foo: ";
    for (std::vector<std::string>::iterator it = v4.begin(); it != v4.end(); ++it)
        out << *it << " ";
    out << std::endl;

    Property p("temp", "tempstring");
    dev.set_prop(p);
    out << p.key << ": " << p << std::endl;

    dev.remove_prop("foo");
    out << "foo: " << dev["foo"] << " (should be 0x0)" << std::endl;

    out << "signal: " << sig << std::endl;

    Signal::List qsig = dev.signals(MPR_DIR_IN);
    qsig.begin();
    for (; qsig != qsig.end(); ++qsig) {
        out << "  input: " << *qsig << std::endl;
    }

    Graph graph(MPR_OBJ);
    Map map(dev.signals(MPR_DIR_OUT)[0], dev.signals(MPR_DIR_IN)[1]);
    map.set_prop(MPR_PROP_EXPR, "y=x[0:1]+123");
    double d[3] = {1., 2., 3.};
    map.signal(MPR_LOC_SRC).set_prop(Property(MPR_PROP_MIN, 0, 3, d));
    map.push();

    while (!map.ready()) {
        dev.poll(100);
    }

    std::vector <double> v(3);
    while (i++ < 100) {
        dev.poll(10);
        graph.poll();
        v[i%3] = i;
        sig.set_value(v);
    }

    // try retrieving linked devices
    out << "devices linked to " << dev << ":" << std::endl;
    Device::List foo = dev[MPR_PROP_LINKED];
    for (; foo != foo.end(); foo++) {
        out << "  " << *foo << std::endl;
    }

    // try combining queries
    out << "devices with name matching 'my*' AND >=0 inputs" << std::endl;
    Device::List qdev = graph.devices();
    qdev.filter(Property("name", "my*"), MPR_OP_EQ);
    qdev.filter(Property("num_inputs", 0), MPR_OP_GTE);
    for (; qdev != qdev.end(); qdev++) {
        out << "  " << *qdev << " (" << (*qdev)[MPR_PROP_NUM_SIGS_IN]
            << " inputs)" << std::endl;
    }

    // check graph records
    out << "graph records:" << std::endl;
    for (const Device d : graph.devices()) {
        out << "  device: " << d << std::endl;
        for (Signal s : d.signals(MPR_DIR_IN)) {
            out << "    input: " << s << std::endl;
        }
        for (Signal s : d.signals(MPR_DIR_OUT)) {
            out << "    output: " << s << std::endl;
        }
    }
    for (Map m : graph.maps()) {
        out << "  map: " << m << std::endl;
    }

    // test some time manipulation
    Time t1(10, 200);
    Time t2(10, 300);
    if (t1 < t2)
        out << "t1 is less than t2" << std::endl;
    t1 += t2;
    if (t1 >= t2)
        out << "(t1 + t2) is greater then or equal to t2" << std::endl;

    printf("\r..................................................Test %s\x1B[0m.\n",
           result ? "\x1B[31mFAILED" : "\x1B[32mPASSED");
    return result;
}
