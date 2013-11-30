# Copyright (c) 2006-2007 The Regents of The University of Michigan
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
# Authors: Lisa Hsu

from m5.objects import *

class L1Cache(BaseCache):
    assoc = 2
    block_size = 64
    hit_latency = '1'
    response_latency = '1'
    mshrs = 10
    tgts_per_mshr = 20
    is_top_level = True

class L2Cache(BaseCache):
    assoc = 8
    block_size = 64
    hit_latency = '10'
    response_latency = '10'
    mshrs = 20
    tgts_per_mshr = 12

class PageTableWalkerCache(BaseCache):
    assoc = 2
    block_size = 64
    hit_latency = '1'
    response_latency = '1'
    mshrs = 10
    size = '1kB'
    tgts_per_mshr = 12
    is_top_level = True

class IOCache(BaseCache):
    assoc = 8
    block_size = 64
    hit_latency = '10'
    response_latency = '10'
    mshrs = 20
    size = '1kB'
    tgts_per_mshr = 12
    forward_snoops = False
    is_top_level = True
