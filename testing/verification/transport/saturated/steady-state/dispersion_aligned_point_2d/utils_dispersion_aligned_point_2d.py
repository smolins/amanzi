import matplotlib.pyplot as plt
import numpy
from amanzi_xml.observations.ObservationXML import ObservationXML as ObsXML
from amanzi_xml.observations.ObservationData import ObservationData as ObsDATA
import amanzi_xml.utils.search as search
import prettytable 
import os, re

def loadInputXML(filename):

    # load input xml file
    #  -- create an ObservationXML object

    Obs_xml = ObsXML(filename)

    return Obs_xml

def loadDataFile(Obs_xml,directory):

    # load the data file
    #  -- use above xml object to get observation filename
    #  -- create an ObservationData object
    #  -- load the data using this object
    
    output_file =  Obs_xml.getObservationFilename()
    Obs_data = ObsDATA(os.path.join(directory,output_file))
    Obs_data.getObservationData()
    coords = Obs_xml.getAllCoordinates()

    for obs in Obs_data.observations.itervalues():
        region = obs.region
        obs.coordinate = coords[region]

    return Obs_data

def CollectObservations(Obs_xml, Obs_data, Obs_lines):

    # Create dictionary for scatter plots
    Obs_scatter={}
    for key in Obs_lines:
        Obs_scatter[key]={}
        Obs_scatter[key]['distance']=[]
        Obs_scatter[key]['Tc99']=[]


    # Collect observations in scatter_data
    for key in Obs_lines:

        if (Obs_lines[key]['slice'][0] is 'x'):
            slice_coord=0
        elif (Obs_lines[key]['slice'][0] is 'y'):
            slice_coord=1
            
        if ( Obs_lines[key]['vary'] is 'x' ):
            vary_coord=0
        elif ( Obs_lines[key]['vary'] is 'y'):
            vary_coord=1

        for obs in Obs_data.observations.itervalues(): 

            if ( obs.coordinate[slice_coord] == Obs_lines[key]['slice'][1] ):
                Obs_scatter[key]['distance'].append(obs.coordinate[vary_coord])
                Obs_scatter[key]['Tc99'].append(obs.data[0])

    return Obs_scatter


def PlotObservations(Obs_scatter,slice_name,subtests,axes1):

    # Plot the observations
    for st in Obs_scatter:
        plot_props=subtests[st]['plot_props']
        axes1.scatter(Obs_scatter[st][slice_name]['distance'],Obs_scatter[st][slice_name]['Tc99'],
                      c=plot_props['color'],marker=plot_props['marker'],s=25,label=plot_props['label']
        )

    return

def MakeTable(Obs_scatter,subtests,analytic_soln,analytic,slice):

    #
    #  API:  Missing
    #
    #  FileName for the output
    #  Table Layout:
    #     - column headings
    #     - "master" column to use for data collection
    #     - keys for data access (subtest,slice)
    #     - Flag to include errors?
    #  Format string for latex
    #  Formatting for Scientific Notation
    #  

    t = prettytable.PrettyTable(["x [m]", "Analytic (AT123D-AT)", "Amanzi First-Order", "Amanzi Second-Order"])
    t.padding_width = 1
    t.hrules = 1
    t.header=True
    t.float_format="10.8"

    table_analytic=[]
    solution=analytic_soln[slice]

    for d in Obs_scatter['amanzi_first']['centerline']['distance']:
        if d in solution['distance']:
            i = solution['distance'].index(d)
            table_analytic.append(solution['c'][i])
        else:
            table_analytic.append("Unavailable")

    tobs=zip( Obs_scatter['amanzi_first']['centerline']['distance'], 
              table_analytic,
              Obs_scatter['amanzi_first']['centerline']['Tc99'],
              Obs_scatter['amanzi_second']['centerline']['Tc99'] )

    tobs.sort(key = lambda t: t[0])

    for x, c_analytic, c_amanzi_1, c_amanzi_2 in tobs:
        t.add_row([ x, c_analytic, c_amanzi_1, c_amanzi_2 ])
        
    table_file = open("table_values.txt", "w+")
    table_file.write(t.get_string())
    table_file.close()

    return


def MakeTableCols(table_layout,slice,
                  Obs_scatter,subtests,analytic_soln,analytic,master_column=None):

    #
    #  API:  Missing
    #
    #  Table Layout:
    #     D column headings
    #     D "master" column to use for data collection
    #     D keys for data access (subtest,slice)
    #     - Flag to include errors?
    #  Format string for latex
    #  Formatting for Scientific Notation
    #  

    #  Create the table
    t = prettytable.PrettyTable()

    #  Local copy of header keys/text
    headers=table_layout[slice]['header']

    #
    #  First (Master) Column
    #
    if master_column is None:
        master_key=headers[0]
        if ( table_layout[slice][master_key]['datasrc'] == 'Amanzi' ):
            st=table_layout[slice][master_key]['subtest']
            var=table_layout[slice][master_key]['variable']
            # Make a copy so we can sort it without messing up relationships to data
            master_data=list(Obs_scatter[st][slice][var])
        master_data.sort()
        t.add_column(master_key, master_data)
        t.float_format[master_key]="4.2"
    else:
        t.add_column(master_column[master_key], master_column[master_data])

    #
    # Second -- Last Columns
    #
    del headers[0]
    for col_key in headers:
        if ( table_layout[slice][col_key]['datasrc'] == 'Amanzi' ):
            st=table_layout[slice][col_key]['subtest']
            var=table_layout[slice][col_key]['variable']
            amanzi_sol=Obs_scatter[st][slice]
            amanzi_data=[]
            for d in master_data:
                if d in amanzi_sol['distance']:
                    i = amanzi_sol['distance'].index(d)
                    amanzi_data.append(amanzi_sol[var][i])
                else:
                    amanzi_data.append("Unavailable")
            t.add_column(col_key, amanzi_data)
            t.float_format[col_key]="10.8"

        elif ( table_layout[slice][col_key]['datasrc'] == 'Analytic' ):
            solution=analytic_soln[slice]
            idepvar=table_layout[slice][col_key]['idepvar']
            var=table_layout[slice][col_key]['variable']
            analytic_data=[]
            for d in master_data:
                if d in solution[idepvar]:
                    i = solution[idepvar].index(d)
                    analytic_data.append(solution[var][i])
                else:
                    analytic_data.append("Unavailable")
            t.add_column(col_key, analytic_data)
            t.float_format[col_key]="10.8"

    #
    #  We could insert columns for particular error / differences here.
    #   

    # Set formatting options
    t.padding_width = 1
    t.hrules = 1
    t.header=True

    # Write the table to a file
    table_file = open(table_layout[slice]['filename'], "w+")
    table_file.write(t.get_string())
    table_file.close()

    return

def CollectAnalyticSolutions(input_file,directory,obs_slice):

    at123d_setup=os.path.join(directory,os.path.splitext(input_file)[0]+"_setup.out")
    at123d_soln=os.path.join(directory,os.path.splitext(input_file)[0]+"_soln.out")

    solution = {}
    solution['source']={}

    #
    #  If symmetry in Y is used, then the source must be centered at the lower bound (0.0). 
    #
    WidthMode=False

    try:
        with open(at123d_setup,'r') as f_setup:
            for line in f_setup:
                if ( WidthMode ):
                    if ( "2 FINITE WIDTH" in line ):
                        WidthMode=False
                        solution['width_mode']=int(re.sub(r'.*. ','', line).rstrip().lstrip())
                elif ( "WIDTH CONTROL" in line ):
                    WidthMode=True
                elif ( "NO. OF POINTS IN X-DIRECTION" in line ):
                    solution['nx']=int(re.sub(r'.*. ','', line).rstrip().lstrip())
                elif ( "NO. OF POINTS IN Y-DIRECTION" in line ):
                    solution['ny']=int(re.sub(r'.*. ','', line).rstrip().lstrip())
                elif ( "NO. OF POINTS IN Z-DIRECTION" in line ):
                    solution['nz']=int(re.sub(r'.*. ','', line).rstrip().lstrip())
                elif ("BEGIN POINT OF X-SOURCE LOCATION" in line):
                    solution['source']['x']=float(re.sub(r'.*. ','', line).rstrip().lstrip())
                elif ("END POINT OF X-SOURCE LOCATION" in line):
                    solution['source']['x']=(solution['source']['x']+float(re.sub(r'.*. ','', line).rstrip().lstrip()))/2.0
                elif ("BEGIN POINT OF Y-SOURCE LOCATION" in line):
                    solution['source']['y']=float(re.sub(r'.*. ','', line).rstrip().lstrip())
                elif ("END POINT OF Y-SOURCE LOCATION" in line and solution['width_mode'] == 2 ):
                    solution['source']['y']=(solution['source']['y']+float(re.sub(r'.*. ','', line).rstrip().lstrip()))/2.0
                elif ("BEGIN POINT OF Z-SOURCE LOCATION" in line):
                    solution['source']['z']=float(re.sub(r'.*. ','', line).rstrip().lstrip())
                elif ("END POINT OF Z-SOURCE LOCATION" in line):
                    solution['source']['z']=(solution['source']['z']+float(re.sub(r'.*. ','', line).rstrip().lstrip()))/2.0
                else:
                    pass

    except IOError:
        raise RuntimeError("Unable to open file"+at123d_setup+", it does not exist OR permissions are incorrect")


    read_x=True
    read_y=True
    read_z=True
    read_t=True
    solution['x']=[]
    solution['y']=[]
    solution['z']=[] 
    solution['c']=[]

    try:
        with open(at123d_soln,'r') as f_soln:
            for line in f_soln:
                if ( read_x ):
                    solution['x']=solution['x']+(' '.join(line.split()).split())
                    if ( len(solution['x']) == solution['nx'] ):
                        read_x=False
                elif ( read_y ):
                    solution['y']=solution['y']+(' '.join(line.split()).split())
                    if ( len(solution['y']) == solution['ny'] ):
                        read_y=False
                elif ( read_z ):
                    solution['z']=solution['z']+(' '.join(line.split()).split())
                    if ( len(solution['z']) == solution['nz'] ):
                        read_z=False
                elif ( read_t ):
                    NL=len(line.split(" "))-1
                    solution['t']=line.split(" ")[NL]
                    read_t=False
                else:
                    solution['c']=solution['c']+(' '.join(line.split()).split())
                    
    except IOError:
        raise RuntimeError("Unable to open file"+at123d_soln+", it does not exist OR permissions are incorrect")

    # Identify the independent variable 
    coord = obs_slice['vary']

    # Convert horizontal axis to float and shift to align the source
    solution['distance'] = [float(i) - float(solution['source'][coord]) for i in solution[coord]]
    solution['c'] = [float(i) for i in solution['c']]
                    
    return solution


def PlotAnalyticSoln(solution, analytic, slice, axes1):

    plot_props = analytic[slice]['plot_props']

    axes1.plot(
        solution[slice]['distance'],solution[slice]['c'],
        label=plot_props['label'],c=plot_props['color'],
    )


def AmanziResults(input_filename,subtests,obs_slices,overwrite=False):
    
    import run_amanzi

    #
    # Create emtpy dictionaries
    #
    obs_scatter={}
    obs_data={}
    obs_xml={}

    try: 

        for st in subtests:

            run_amanzi.run_amanzi(input_filename, subtests[st]['directory'], subtests[st]['parameters'], subtests[st]['mesh_file'],overwrite)
            obs_xml[st]=loadInputXML(input_filename)
            obs_data[st]=loadDataFile(obs_xml[st],subtests[st]['directory'])

            # Collect observations to plot
            obs_scatter[st]=CollectObservations(obs_xml[st], obs_data[st], obs_slices)
            
    finally: 
        pass

    return obs_xml, obs_data, obs_scatter


def AnalyticSolutions(analytic,obs_slices,overwrite=False):

    import run_at123d_at

    analytic_soln={}

    try:

        for a in analytic:
            run_at123d_at.run_at123d(analytic[a]['input_file'], analytic[a]['directory'],overwrite)
            analytic_soln[a]=CollectAnalyticSolutions(analytic[a]['input_file'],analytic[a]['directory'],obs_slices[a])

    finally:
        pass

    return analytic_soln


def SetupTests():

    # Collect slices of concentration from the observations
    #
    # Slice should be all fixed quantities, i.e., Time is fixed as well 
    # Should include observation type (or return slices sorted with observation type as a key).

    obs_slices = { 'centerline'  : {'slice': [ 'y', 0.0 ], 'vary': 'x', 'domain': [-270.0,960.0] },
                   'x=0.0'       : {'slice': [ 'x', 0.0 ], 'vary': 'y', 'domain': [-5.0,270.0] },
                   'x=424.0'     : {'slice': [ 'x', 424.0 ], 'vary': 'y', 'domain': [-5.0,270.0] },
    }


    subtests = { 'amanzi_first' : 
                 { 'directory'  : 'amanzi-output-first-order',
                   'mesh_file'  : '../amanzi_dispersion_aligned_point_2d.exo',
                   'parameters' : { 'Transport Integration Algorithm': 'Explicit First-Order' },
                   'plot_props' : { 'marker':'s', 'color':'r', 'label': 'Amanzi: First Order' } 
                 },
                 'amanzi_second' : 
                 { 'directory'  : 'amanzi-output-second-order',
                   'mesh_file'  : '../amanzi_dispersion_aligned_point_2d.exo',
                   'parameters' : { 'Transport Integration Algorithm': "Explicit Second-Order" },
                   'plot_props' : { 'marker':'o', 'color':'b', 'label': 'Amanzi: Second Order' }
                 },
             }
   

    analytic = { 'centerline'   : 
                 { 'directory'  : 'at123d-at',
                   'input_file' : 'at123d-at_centerline.list',
                   'plot_props' : { 'color': 'blue', 'linestyle' : '-', 'label':'Analytic (AT123D-AT)' }
                 },
                 'x=0.0'      : 
                 { 'directory' : 'at123d-at', 
                   'input_file' : 'at123d-at_slice_x=0.list', 
                   'plot_props' : { 'color': 'blue', 'linestyle' : '-', 'label':'Analytic (AT123D-AT)' }
                 },
                 'x=424.0'      :   
                 { 'directory'  : 'at123d-at', 
                   'input_file' : 'at123d-at_slice_x=420.list',
                   'plot_props' : { 'color': 'blue', 'linestyle' : '-', 'label':'Analytic (AT123D-AT)' }
                 },
               }


    table_layout={ 'centerline' : 
                   { 'filename' : 'table_centerline.txt',
                     'header'   : [ 'x [m]', 'Analytic (AT123D-AT)',
                                    'Amanzi First-Order', 'Amanzi Second-Order'],
                     'x [m]'    : 
                     { 'datasrc' : 'Amanzi', 'subtest' : 'amanzi_first', 'variable': 'distance' }, 
                     'Amanzi First-Order' :
                     { 'datasrc' : 'Amanzi', 'subtest' : 'amanzi_first', 'variable': 'Tc99' },
                     'Amanzi Second-Order' : 
                     { 'datasrc' : 'Amanzi', 'subtest' : 'amanzi_second', 'variable': 'Tc99' },
                     'Analytic (AT123D-AT)':
                     { 'datasrc' : 'Analytic', 'idepvar' : 'distance', 'variable': 'c' },
                 },
                   'x=0.0' : 
                   { 'filename' : 'table_cross-section-b.txt',
                     'header'   : [ 'y [m]', 'Analytic (AT123D-AT)',
                                    'Amanzi First-Order', 'Amanzi Second-Order'],
                     'y [m]'    : 
                     { 'datasrc' : 'Amanzi', 'subtest' : 'amanzi_first', 'variable': 'distance' }, 
                     'Amanzi First-Order' :
                     { 'datasrc' : 'Amanzi', 'subtest' : 'amanzi_first', 'variable': 'Tc99' },
                     'Amanzi Second-Order' : 
                     { 'datasrc' : 'Amanzi', 'subtest' : 'amanzi_second', 'variable': 'Tc99' },
                     'Analytic (AT123D-AT)':
                     { 'datasrc' : 'Analytic', 'idepvar' : 'distance', 'variable': 'c' },
                 },
                   'x=424.0' : 
                   { 'filename' : 'table_cross-section-c.txt',
                     'header'   : [ 'y [m]', 'Analytic (AT123D-AT)',
                                    'Amanzi First-Order', 'Amanzi Second-Order'],
                     'y [m]'    : 
                     { 'datasrc' : 'Amanzi', 'subtest' : 'amanzi_first', 'variable': 'distance' }, 
                     'Amanzi First-Order' :
                     { 'datasrc' : 'Amanzi', 'subtest' : 'amanzi_first', 'variable': 'Tc99' },
                     'Amanzi Second-Order' : 
                     { 'datasrc' : 'Amanzi', 'subtest' : 'amanzi_second', 'variable': 'Tc99' },
                     'Analytic (AT123D-AT)':
                     { 'datasrc' : 'Analytic', 'idepvar' : 'distance', 'variable': 'c' },
                 }

               }
                   

    return obs_slices, subtests, analytic, table_layout
