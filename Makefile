CXX=clang++
CXXFLAGS=-std=c++11 -Wall -pedantic
BOOST_FLAGS=-I/usr/local/include -L/usr/local/lib -pthread -lboost_system -lboost_regex

all: socks_server hw4.cgi

socks_server: socks_server.cc
	$(CXX) $? -o $@ $(CXXFLAGS)

hw4.cgi: console.cc
	$(CXX) $? -o $@ $(CXXFLAGS) $(BOOST_FLAGS)

.PHONY: clean
clean:
	rm -rf socks_server hw4.cgi

.PHONY: format
format:
	clang-format -i *.cc

.PHONY: check
check:
	clang-tidy -checks='bugprone-*,clang-analyzer-*,modernize-*' *.cc -- -std=c++11
